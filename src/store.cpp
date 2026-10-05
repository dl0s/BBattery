#include "store.h"
#include "capacity.h"
#include "capacity_summary.h"
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QUuid>
#include <unistd.h>
#include <sys/statvfs.h>
#include <algorithm>

namespace Battery {
namespace {
void bind(sqlite3_stmt *s,const QVariantList &values){
    for(int i=0;i<values.size();++i){
        const QVariant &v=values[i];
        if(!v.isValid() || v.isNull()) sqlite3_bind_null(s,i+1);
        else if(v.type()==QVariant::String) {
            QByteArray text=v.toString().toUtf8();sqlite3_bind_text(s,i+1,text.constData(),text.size(),SQLITE_TRANSIENT);
        } else if(v.type()==QVariant::Double) sqlite3_bind_double(s,i+1,v.toDouble());
        else sqlite3_bind_int64(s,i+1,v.toLongLong());
    }
}
QVariantMap readRow(sqlite3_stmt *s){
    QVariantMap row;
    for(int i=0;i<sqlite3_column_count(s);++i){
        QString name=QString::fromUtf8(sqlite3_column_name(s,i));int type=sqlite3_column_type(s,i);
        if(type==SQLITE_NULL)row[name]=QVariant();
        else if(type==SQLITE_INTEGER)row[name]=QVariant(qint64(sqlite3_column_int64(s,i)));
        else if(type==SQLITE_FLOAT)row[name]=sqlite3_column_double(s,i);
        else row[name]=QString::fromUtf8(reinterpret_cast<const char*>(sqlite3_column_text(s,i)));
    }
    return row;
}
void flushBucket(QVariantList &out,QVariantList &bucket,bool mixed){
    if(bucket.isEmpty())return;
    QList<int> indices;indices<<0<<bucket.size()-1;
    const char *fields[]={"soc","current","voltage","temperature"};
    for(int f=0;f<4;++f){
        int lo=-1,hi=-1;double minimum=1e30,maximum=-1e30;
        for(int i=0;i<bucket.size();++i){
            const QVariantMap row=bucket[i].toMap();QVariant v=row[fields[f]];
            if(!v.isValid()||v.isNull())continue;
            double value=v.toDouble();
            if(lo<0||value<minimum){lo=i;minimum=value;}
            if(hi<0||value>maximum){hi=i;maximum=value;}
        }
        if(lo>=0)indices<<lo<<hi;
    }
    std::sort(indices.begin(),indices.end());int last=-1;
    foreach(int i,indices)if(i!=last){
        QVariantMap row=bucket[i].toMap();
        if(mixed){
            row["display_break"]=true;
            if(row["gap_reason"].toString().isEmpty())row["gap_reason"]="display-bucket-break";
        }
        row["display_decimated"]=true;
        out<<row;last=i;
    }
    bucket.clear();
}
}
Store::Store():db(0),havePrevious(false),session(0),count(0),startSoc(-1),lastSoc(-1),
    stableSoc(true),mah(0),mwh(0),covered(0),energyCovered(0),startMono(0),batteryKey("legacy"){}
Store::~Store(){if(db)sqlite3_close(db);}
bool Store::execute(const QString &sql,const QVariantList &values){
    sqlite3_stmt *s=0;
    if(sqlite3_prepare_v2(db,sql.toUtf8().constData(),-1,&s,0)!=SQLITE_OK){failure=QString::fromUtf8(sqlite3_errmsg(db));return false;}
    bind(s,values);int rc=sqlite3_step(s);sqlite3_finalize(s);
    if(rc!=SQLITE_DONE && rc!=SQLITE_ROW){failure=QString::fromUtf8(sqlite3_errmsg(db));return false;}
    return true;
}
QVariantList Store::query(const QString &sql,const QVariantList &values){
    QVariantList rows;sqlite3_stmt *s=0;
    if(!db || sqlite3_prepare_v2(db,sql.toUtf8().constData(),-1,&s,0)!=SQLITE_OK){failure=db?QString::fromUtf8(sqlite3_errmsg(db)):"Database unavailable";return rows;}
    bind(s,values);int rc;
    while((rc=sqlite3_step(s))==SQLITE_ROW){
        rows<<readRow(s);
    }
    if(rc!=SQLITE_DONE) failure=QString::fromUtf8(sqlite3_errmsg(db));
    sqlite3_finalize(s);return rows;
}
bool Store::open(const QString &path,bool writer){
    if(db) return true;
    databasePath=path;
    int flags=writer?(SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE):SQLITE_OPEN_READONLY;
    if(sqlite3_open_v2(QFile::encodeName(path).constData(),&db,flags,0)!=SQLITE_OK){
        failure=db?QString::fromUtf8(sqlite3_errmsg(db)):"Database open failed";if(db)sqlite3_close(db);db=0;return false;
    }
    sqlite3_busy_timeout(db,writer?2000:500);
    if(!writer) return execute("PRAGMA query_only=ON");
    const qint64 pages=query("PRAGMA page_count").value(0).toMap().value("page_count").toLongLong();
    const qint64 pageSize=query("PRAGMA page_size").value(0).toMap().value("page_size").toLongLong();
    if(pageSize<=0||!execute(QString("PRAGMA max_page_count=%1").arg(qMax(pages,Q_INT64_C(67108864)/pageSize))))return false;
    // DELETE journaling supports read-only clients on this legacy SQLite runtime.
    const char *schema=
        "PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL; PRAGMA foreign_keys=ON; BEGIN IMMEDIATE;"
        "CREATE TABLE IF NOT EXISTS metadata(key TEXT PRIMARY KEY,value TEXT);"
        "INSERT OR IGNORE INTO metadata VALUES('schema','1');"
        "CREATE TABLE IF NOT EXISTS snapshots(id INTEGER PRIMARY KEY,raw TEXT NOT NULL UNIQUE);"
        "CREATE TABLE IF NOT EXISTS sessions(id INTEGER PRIMARY KEY,start_ms INTEGER NOT NULL,last_ms INTEGER NOT NULL,"
        "end_ms INTEGER,mode TEXT NOT NULL,battery_id INTEGER,reason TEXT,start_soc INTEGER,end_soc INTEGER,"
        "sample_count INTEGER DEFAULT 0,mah REAL DEFAULT 0,mwh REAL DEFAULT 0,covered_s REAL DEFAULT 0,"
        "energy_covered_s REAL DEFAULT 0,elapsed_s REAL DEFAULT 0,stable_soc INTEGER DEFAULT 1);"
        "CREATE TABLE IF NOT EXISTS samples(id INTEGER PRIMARY KEY,utc_ms INTEGER NOT NULL,mono_ms INTEGER NOT NULL,"
        "run_id TEXT NOT NULL,session_id INTEGER NOT NULL REFERENCES sessions(id),snapshot_id INTEGER REFERENCES snapshots(id),"
        "interval_s INTEGER,ready INTEGER,battery_id INTEGER,mode TEXT,charger TEXT,phase TEXT,"
        "soc REAL,current REAL,voltage REAL,temperature REAL,design REAL,full_capacity REAL,remaining REAL,"
        "health REAL,cycles REAL,input_limit REAL,charge_limit REAL,input_current REAL,"
        "source_changed INTEGER,gap_reason TEXT,error TEXT);"
        "CREATE INDEX IF NOT EXISTS samples_time ON samples(utc_ms);"
        "CREATE INDEX IF NOT EXISTS samples_session ON samples(session_id,utc_ms);"
        "CREATE INDEX IF NOT EXISTS samples_session_order ON samples(session_id,id);"
        "CREATE INDEX IF NOT EXISTS sessions_battery_mode ON sessions(battery_id,mode,id);"
        "CREATE INDEX IF NOT EXISTS sessions_reason ON sessions(reason,id);"
        "CREATE TABLE IF NOT EXISTS battery_profiles(key TEXT PRIMARY KEY,label TEXT NOT NULL);"
        "INSERT OR IGNORE INTO battery_profiles VALUES('legacy','电池 1');"
        "CREATE TABLE IF NOT EXISTS session_batteries(session_id INTEGER PRIMARY KEY REFERENCES sessions(id),"
        "battery_key TEXT NOT NULL REFERENCES battery_profiles(key));"
        "CREATE INDEX IF NOT EXISTS session_batteries_key ON session_batteries(battery_key,session_id);"
        "INSERT OR IGNORE INTO session_batteries SELECT id,'legacy' FROM sessions "
        "WHERE NOT EXISTS (SELECT 1 FROM metadata WHERE key='battery_profiles_migrated');"
        "INSERT OR IGNORE INTO metadata VALUES('battery_profiles_migrated','1');"
        "CREATE TABLE IF NOT EXISTS events(id INTEGER PRIMARY KEY,utc_ms INTEGER NOT NULL,kind TEXT NOT NULL,"
        "text TEXT NOT NULL,session_id INTEGER,sample_id INTEGER);"
        "CREATE INDEX IF NOT EXISTS events_time ON events(utc_ms);"
        "CREATE TABLE IF NOT EXISTS capacity_tests(id TEXT PRIMARY KEY,battery_key TEXT NOT NULL REFERENCES battery_profiles(key),"
        "start_ms INTEGER NOT NULL,start_mono INTEGER NOT NULL,deadline_mono INTEGER NOT NULL,requested_s INTEGER NOT NULL,"
        "last_ms INTEGER NOT NULL,last_mono INTEGER NOT NULL,end_ms INTEGER,status TEXT NOT NULL,reason TEXT,mode TEXT NOT NULL,"
        "samples INTEGER DEFAULT 0,discharge_mah REAL DEFAULT 0,charge_mah REAL DEFAULT 0,discharge_mwh REAL DEFAULT 0,charge_mwh REAL DEFAULT 0,"
        "integrated_s REAL DEFAULT 0,energy_s REAL DEFAULT 0,elapsed_s REAL DEFAULT 0,start_soc REAL,end_soc REAL,stable_soc INTEGER DEFAULT 1,"
        "gaps INTEGER DEFAULT 0,capacity REAL);"
        "CREATE INDEX IF NOT EXISTS capacity_tests_battery ON capacity_tests(battery_key,start_ms);"
        "CREATE INDEX IF NOT EXISTS capacity_tests_status ON capacity_tests(status);"
        "CREATE INDEX IF NOT EXISTS capacity_tests_summary_fallback ON capacity_tests(battery_key,mode,status,end_ms DESC,id DESC);"
        "CREATE TABLE IF NOT EXISTS test_samples(test_id TEXT NOT NULL REFERENCES capacity_tests(id),"
        "sample_id INTEGER NOT NULL UNIQUE REFERENCES samples(id),PRIMARY KEY(test_id,sample_id));"
        "CREATE TABLE IF NOT EXISTS test_control(id INTEGER PRIMARY KEY CHECK(id=1),request_id TEXT,ok INTEGER,message TEXT);"
        "CREATE TABLE IF NOT EXISTS collector_state(id INTEGER PRIMARY KEY CHECK(id=1),run_id TEXT,utc_ms INTEGER,"
        "mono_ms INTEGER,interval_s INTEGER,paused INTEGER,error TEXT,pid INTEGER,euid INTEGER); COMMIT;";
    char *err=0;
    if(sqlite3_exec(db,schema,0,0,&err)!=SQLITE_OK){failure=QString::fromUtf8(err?err:"Schema failed");sqlite3_free(err);return false;}
    // Optional acceleration on older installed SQLite; the ordinary index remains valid.
    if(sqlite3_libversion_number()>=3008000&&!execute(QString::fromUtf8(Capacity::summaryIndex().c_str())))return false;
    QVariantList version=query("SELECT value FROM metadata WHERE key='schema'");
    if(version.isEmpty() || version.first().toMap()["value"]!="1"){failure="Unsupported database schema";return false;}
    if(!query("SELECT id FROM samples LIMIT 1").isEmpty())pendingBreak="collector-restart";
    if(!execute("UPDATE capacity_tests SET status='interrupted',reason='collector-restart',end_ms=last_ms,capacity=NULL WHERE status='running'"))return false;
    return execute("UPDATE sessions SET end_ms=last_ms,reason='collector-restart' WHERE end_ms IS NULL");
}
QVariantMap Store::capacitySummary(const QString &key,const QString &activeKey,const QVariantMap &systemSample){
    QVariantMap summary;summary["value"]=QVariant();summary["unit"]="mAh";
    summary["sourceType"]="none";summary["sourceId"]=QVariant();summary["estimatedAt"]=QVariant();
    summary["available"]=false;summary["unavailableReason"]="no-qualified-test";
    summary["discharge"]=QVariantMap();summary["charge"]=QVariantMap();
    const char *modes[]={"discharge","charge"};
    for(int i=0;i<2;++i){
        QVariantList rows=query(QString::fromUtf8(Capacity::summarySql().c_str()),QVariantList()<<key<<modes[i]);
        if(!failure.isEmpty()){summary["unavailableReason"]="database-read-failed";return summary;}
        if(rows.isEmpty())continue;
        const QVariantMap row=rows.first().toMap();QVariantMap estimate;
        estimate["value"]=row["capacity"];estimate["sourceId"]=row["id"];estimate["estimatedAt"]=row["end_ms"];
        summary[modes[i]]=estimate;
        if(!summary["available"].toBool()){
            summary["value"]=estimate["value"];summary["sourceId"]=estimate["sourceId"];summary["estimatedAt"]=estimate["estimatedAt"];
            summary["sourceType"]=QString(modes[i])+"-test";summary["available"]=true;summary["unavailableReason"]=QVariant();
        }
    }
    // Historical samples have a software marker, not proof of the installed cell.
    // Accept BPS full-charge capacity only with an explicit confirmed attribution token.
    if(!summary["available"].toBool()&&Capacity::systemReference(key==activeKey&&systemSample["battery_key"]==key,
       systemSample["attributionConfirmed"].toBool(),systemSample["ready"].toBool(),systemSample["source"]=="bps-full-charge-capacity",
       systemSample["id"].toLongLong()>0,systemSample["utc_ms"].toLongLong()>0,systemSample["full_capacity"].toDouble())){
        summary["value"]=systemSample["full_capacity"];summary["sourceType"]="system-reference";
        summary["sourceId"]=systemSample["id"];summary["estimatedAt"]=systemSample["utc_ms"];
        summary["available"]=true;summary["unavailableReason"]=QVariant();
    }
    return summary;
}
QVariantMap Store::capacitySession(const QString &mode,int batteryId,qint64 sessionId,const QString &key){
    if((batteryId<0&&key.isEmpty())||(mode!="charge"&&mode!="discharge"))return QVariantMap();
    QVariantList bindings;bindings<<mode<<(key.isEmpty()?QVariant(batteryId):QVariant(key))<<sessionId<<sessionId;
    if(key.isEmpty())bindings<<sessionId;
    QString sql=QString::fromUtf8(Capacity::historySql(!key.isEmpty(),false).c_str());
    sql.replace("ORDER BY s.id DESC LIMIT 1","AND s.id<? ORDER BY s.id DESC LIMIT 1");
    qint64 before=Q_INT64_C(9223372036854775807);
    for(;;){
        QVariantList values=bindings;values<<before;QVariantList rows=query(sql,values);
        if(rows.isEmpty())return QVariantMap();
        QVariantMap row=rows.first().toMap();before=row["id"].toLongLong();
        Capacity::SocSequence sequence(mode=="charge");sqlite3_stmt *probe=0;
        // One ordered pass replaces a correlated previous-SOC lookup for every sample.
        if(sqlite3_prepare_v2(db,"SELECT id,soc FROM samples WHERE session_id=? AND id>? AND soc IS NOT NULL ORDER BY id LIMIT 512",
            -1,&probe,0)!=SQLITE_OK){failure=QString::fromUtf8(sqlite3_errmsg(db));return QVariantMap();}
        qint64 cursor=0;bool stable=true,complete=true;
        while(stable){
            sqlite3_bind_int64(probe,1,before);sqlite3_bind_int64(probe,2,cursor);int rc,n=0;
            while((rc=sqlite3_step(probe))==SQLITE_ROW){
                cursor=sqlite3_column_int64(probe,0);++n;
                if(!sequence.add(sqlite3_column_double(probe,1))){stable=false;break;}
            }
            if(stable&&rc!=SQLITE_DONE){failure=QString::fromUtf8(sqlite3_errmsg(db));complete=false;}
            sqlite3_reset(probe);
            if(!complete||n<512||!stable)break;
            usleep(1000);
        }
        sqlite3_finalize(probe);
        if(!complete)return QVariantMap();
        if(sequence.valid())return row;
    }
}
bool Store::registerBattery(const QString &key,const QString &label){
    if(key.isEmpty()||label.trimmed().isEmpty())return false;
    if(!execute("INSERT OR IGNORE INTO battery_profiles VALUES(?,?)",QVariantList()<<key<<label))return false;
    return execute("UPDATE battery_profiles SET label=? WHERE key=? AND label<>?",QVariantList()<<label<<key<<label);
}
bool Store::setBattery(const QString &key,const QString &label){
    if(!registerBattery(key,label))return false;
    if(key==batteryKey)return true;
    if(!closeSession("battery-marker-changed"))return false;
    batteryKey=key;pendingBreak="battery-marker-changed";return true;
}
bool Store::beginSession(const Sample &s){
    startSoc=s.has("soc")?int(s.value("soc")):-1;lastSoc=startSoc;count=0;mah=0;mwh=0;
    covered=0;energyCovered=0;stableSoc=true;startMono=s.mono;
    QVariant soc=startSoc>=0?QVariant(startSoc):QVariant();
    if(!execute("INSERT INTO sessions(start_ms,last_ms,mode,battery_id,start_soc,end_soc) VALUES(?,?,?,?,?,?)",
       QVariantList()<<s.utc<<s.utc<<s.mode<<s.batteryId<<soc<<soc))return false;
    session=sqlite3_last_insert_rowid(db);
    return execute("INSERT INTO session_batteries VALUES(?,?)",QVariantList()<<session<<batteryKey);
}
bool Store::closeSession(const QString &reason){
    if(!session){havePrevious=false;return true;}
    if(!execute("UPDATE sessions SET end_ms=last_ms,reason=? WHERE id=?",QVariantList()<<reason<<session))return false;
    session=0;havePrevious=false;
    if(reason=="paused"||reason=="collector-stopped")pendingBreak=reason;
    return true;
}
bool Store::append(const Sample &s){
    const Sample savedPrevious=previous;const bool savedHave=havePrevious;const qint64 savedSession=session;
    const int savedCount=count,savedStartSoc=startSoc,savedLastSoc=lastSoc;const bool savedStable=stableSoc;
    const double savedMah=mah,savedMwh=mwh,savedCovered=covered,savedEnergyCovered=energyCovered;
    const qint64 savedStartMono=startMono;
    const QString savedBreak=pendingBreak;
    Step step;if(havePrevious)step=integrate(previous,s);
    QString breakReason=havePrevious?step.breakReason:pendingBreak;
    bool ok=execute("SAVEPOINT append_sample");
    if(!ok)return false;
    if(session && (!breakReason.isEmpty() || previous.mode!=s.mode))
        ok=closeSession(breakReason.isEmpty()?"state-changed":breakReason);
    if(ok && !session)ok=beginSession(s);
    if(ok)ok=execute("INSERT OR IGNORE INTO snapshots(raw) VALUES(?)",QVariantList()<<s.raw);
    QVariantList snapshot=ok?query("SELECT id FROM snapshots WHERE raw=?",QVariantList()<<s.raw):QVariantList();
    if(snapshot.isEmpty())ok=false;
    qint64 snapshotId=snapshot.isEmpty()?0:snapshot.first().toMap()["id"].toLongLong();
    bool changed=!havePrevious || previous.raw!=s.raw;
    if(ok && havePrevious && step.chargeValid){mah+=step.charge;covered+=step.seconds;}
    if(ok && havePrevious && step.energyValid){mwh+=step.energy;energyCovered+=step.seconds;}
    if(s.has("soc")){
        int soc=int(s.value("soc"));
        if(lastSoc>=0&&(s.mode=="charge"||s.mode=="discharge")&&
           !Capacity::followsSoc(s.mode=="charge",lastSoc,soc))stableSoc=false;
        lastSoc=soc;
    }
    QVariantList v;v<<s.utc<<s.mono<<s.run<<session<<snapshotId<<s.interval<<int(s.ready)<<s.batteryId<<s.mode<<s.charger<<s.phase;
    const char *metrics[]={"soc","current","voltage","temperature","design","full_capacity","remaining",
        "health","cycles","input_limit","charge_limit","input_current"};
    for(unsigned i=0;i<sizeof(metrics)/sizeof(metrics[0]);++i)v<<s.values.value(metrics[i]);
    v<<int(changed)<<breakReason<<s.error;
    if(ok)ok=execute("INSERT INTO samples(utc_ms,mono_ms,run_id,session_id,snapshot_id,interval_s,ready,battery_id,"
        "mode,charger,phase,soc,current,voltage,temperature,design,full_capacity,remaining,health,cycles,input_limit,"
        "charge_limit,input_current,source_changed,gap_reason,error) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",v);
    if(ok&&!testId.isEmpty())ok=appendTest(s,savedPrevious,savedHave,step,sqlite3_last_insert_rowid(db));
    ++count;
    if(ok)ok=execute("UPDATE sessions SET last_ms=?,end_soc=?,sample_count=?,mah=?,mwh=?,covered_s=?,energy_covered_s=?,"
        "elapsed_s=?,stable_soc=? WHERE id=?",QVariantList()<<s.utc<<(lastSoc>=0?QVariant(lastSoc):QVariant())<<count<<mah<<mwh
        <<covered<<energyCovered<<qMax(0.0,(s.mono-startMono)/1000.0)<<int(stableSoc)<<session);
    if(ok&&savedHave&&savedPrevious.mode!=s.mode)ok=recordEvent(s,"mode-"+s.mode,modeLabel(s.mode));
    if(ok&&savedHave&&connected(savedPrevious.charger)!=connected(s.charger))
        ok=recordEvent(s,connected(s.charger)?"power-connected":"power-disconnected",
            QString::fromUtf8(connected(s.charger)?"接入外部电源":"断开外部电源"));
    if(ok&&breakReason=="battery-marker-changed")ok=recordEvent(s,"battery-marker-changed",QString::fromUtf8("切换电池标记"));
    if(ok)ok=execute("RELEASE append_sample");
    if(!ok){
        execute("ROLLBACK TO append_sample");execute("RELEASE append_sample");previous=savedPrevious;havePrevious=savedHave;session=savedSession;
        count=savedCount;startSoc=savedStartSoc;lastSoc=savedLastSoc;stableSoc=savedStable;
        mah=savedMah;mwh=savedMwh;covered=savedCovered;energyCovered=savedEnergyCovered;startMono=savedStartMono;
        pendingBreak=savedBreak;
        return false;
    }
    previous=s;havePrevious=true;pendingBreak.clear();return true;
}
bool Store::recordEvent(const Sample &s,const QString &kind,const QString &text,int cooldown){
    if(cooldown>0){
        QVariantList last=query("SELECT MAX(utc_ms) AS stamp FROM events WHERE kind=?",QVariantList()<<kind);
        QVariant stamp=last.isEmpty()?QVariant():last.first().toMap()["stamp"];
        if(stamp.isValid()&&!stamp.isNull()&&s.utc-stamp.toLongLong()<qint64(cooldown)*1000)return true;
    }
    return execute("INSERT INTO events(utc_ms,kind,text,session_id,sample_id) VALUES(?,?,?,?,"
        "(SELECT MAX(id) FROM samples))",QVariantList()<<s.utc<<kind<<text<<session);
}
QVariantList Store::history(qint64 begin,qint64 end,QVariantMap *summary,qint64 sessionId,const QString &key){
    HistorySummary stats(begin,end);QVariantList plot,bucket;
    QVariantList maximum=query("SELECT MAX(id) AS last_id FROM samples");
    qint64 maximumId=maximum.isEmpty()?0:maximum.first().toMap()["last_id"].toLongLong();
    QString scope=sessionId?QString(" AND session_id=%1").arg(sessionId):QString();
    if(!key.isEmpty()){
        QString escaped=key;escaped.replace("'","''");
        scope+=" AND session_id IN (SELECT session_id FROM session_batteries WHERE battery_key='"+escaped+"')";
    }
    scope+=QString(" AND id<=%1").arg(maximumId);
    QVariantList prior=query("SELECT * FROM samples WHERE utc_ms<?"+scope+" ORDER BY utc_ms DESC,id DESC LIMIT 1",QVariantList()<<begin);
    if(!prior.isEmpty())stats.add(prior.first().toMap());
    qint64 width=qMax(qint64(1),(end-begin)/360),index=-1;bool mixed=false;QVariantMap previousRow;
    qint64 cursorTime=begin,cursorId=-1;bool complete=failure.isEmpty();
    // Short read statements let the independent writer commit between pages.
    while(complete){
        sqlite3_stmt *s=0;
        QString sql="SELECT * FROM samples WHERE utc_ms>=? AND utc_ms<=?"+scope+
            " AND (utc_ms>? OR id>?) ORDER BY utc_ms,id LIMIT 512";
        if(!db||sqlite3_prepare_v2(db,sql.toUtf8().constData(),-1,&s,0)!=SQLITE_OK){complete=false;break;}
        bind(s,QVariantList()<<cursorTime<<end<<cursorTime<<cursorId);int rc,count=0;
        while((rc=sqlite3_step(s))==SQLITE_ROW){
            QVariantMap row=readRow(s);stats.add(row);++count;
            cursorTime=row["utc_ms"].toLongLong();cursorId=row["id"].toLongLong();
            qint64 slot=(cursorTime-begin)/width;
            if(slot!=index){flushBucket(plot,bucket,mixed);index=slot;mixed=false;previousRow.clear();}
            if(!row["gap_reason"].toString().isEmpty()||!row["ready"].toBool()||
                (!previousRow.isEmpty()&&(row["session_id"]!=previousRow["session_id"]||
                row["run_id"]!=previousRow["run_id"]||
                cursorTime-previousRow["utc_ms"].toLongLong()>qMax(row["interval_s"].toInt(),previousRow["interval_s"].toInt())*3000)))mixed=true;
            const char *required[]={"soc","current","voltage","temperature"};
            for(int i=0;i<4;++i)if(row[required[i]].isNull())mixed=true;
            bucket<<row;previousRow=row;
        }
        complete=rc==SQLITE_DONE;
        if(!complete)failure=QString::fromUtf8(sqlite3_errmsg(db));
        sqlite3_finalize(s);
        if(!complete||count<512)break;
        usleep(1000);
    }
    flushBucket(plot,bucket,mixed);
    QVariantList after=query("SELECT * FROM samples WHERE utc_ms>?"+scope+" ORDER BY utc_ms,id LIMIT 1",QVariantList()<<end);
    if(!after.isEmpty())stats.add(after.first().toMap());
    complete=complete&&failure.isEmpty();
    if(summary){*summary=stats.result();(*summary)["complete"]=complete;}
    return plot;
}
bool Store::heartbeat(const QString &run,int interval,bool paused,const QString &error){
    return execute("INSERT OR REPLACE INTO collector_state VALUES(1,?,?,?,?,?,?,?,?)",
        QVariantList()<<run<<utcMillis()<<monoMillis()<<interval<<int(paused)<<error<<qint64(getpid())<<qint64(geteuid()));
}

bool Store::startTest(const QString &id,const QString &key,const QString &label,int seconds,const Sample &first){
    const qint64 pages=query("PRAGMA page_count").value(0).toMap().value("page_count").toLongLong();
    const qint64 pageSize=query("PRAGMA page_size").value(0).toMap().value("page_size").toLongLong();
    struct statvfs space;
    if(pages*pageSize>=Q_INT64_C(62914560)||statvfs(QFile::encodeName(databasePath).constData(),&space)!=0
       ||quint64(space.f_bavail)*quint64(space.f_frsize)<Q_UINT64_C(16777216)){
        failure="History or free-space budget exceeded; export history before a new test";return false;
    }
    if(id.isEmpty()||seconds<60||seconds>86400||!testId.isEmpty()||!first.ready||
       !first.has("current")||(first.mode!="charge"&&first.mode!="discharge")){
        failure="Battery must be charging or discharging; duration must be 1-1440 minutes";return false;
    }
    Store saved=*this;saved.db=0;
    if(!execute("SAVEPOINT start_test"))return false;
    bool ok=setBattery(key,label)&&closeSession("test-started");
    if(ok)ok=execute("INSERT INTO capacity_tests(id,battery_key,start_ms,start_mono,deadline_mono,requested_s,last_ms,last_mono,status,mode,start_soc,end_soc) "
        "VALUES(?,?,?,?,?,?,?,?,'running',?,?,?)",QVariantList()<<id<<key<<first.utc<<first.mono
        <<first.mono+qint64(seconds)*1000<<seconds<<first.utc<<first.mono<<first.mode<<first.values.value("soc")<<first.values.value("soc"));
    if(ok){testId=id;ok=append(first);}
    if(ok)ok=execute("INSERT OR REPLACE INTO test_control VALUES(1,?,1,?)",QVariantList()<<id<<QString::fromUtf8("测试已开始"));
    if(ok)ok=execute("RELEASE start_test");
    if(!ok){QString error=failure;execute("ROLLBACK TO start_test");execute("RELEASE start_test");sqlite3 *connection=db;*this=saved;db=connection;failure=error;}
    return ok;
}
qint64 Store::testDeadline()const{
    // The collector owns this connection and reads only one indexed row.
    QVariantList rows=const_cast<Store*>(this)->query("SELECT deadline_mono FROM capacity_tests WHERE id=?",QVariantList()<<testId);
    return rows.isEmpty()?0:rows.first().toMap()["deadline_mono"].toLongLong();
}
bool Store::appendTest(const Sample &s,const Sample &prior,bool havePrior,const Step &step,qint64 sampleId){
    QVariantList rows=query("SELECT * FROM capacity_tests WHERE id=? AND status='running'",QVariantList()<<testId);
    if(rows.isEmpty()){failure="Running test unavailable";return false;}
    QVariantMap t=rows.first().toMap();
    const qint64 deadline=t["deadline_mono"].toLongLong();
    const qint64 endpoint=qMin(s.mono,deadline);
    const double elapsed=qMax(0.0,(endpoint-t["start_mono"].toLongLong())/1000.0);
    const double fraction=havePrior&&s.mono>prior.mono?qBound(0.0,double(endpoint-prior.mono)/(s.mono-prior.mono),1.0):1.0;
    bool stable=t["stable_soc"].toBool()&&s.ready&&s.has("soc")&&s.mode==t["mode"].toString();
    if(havePrior&&!step.breakReason.isEmpty()&&step.breakReason!="sampling-gap")stable=false;
    if(havePrior&&prior.has("soc")&&s.has("soc")){
        double a=prior.value("soc"),b=s.value("soc");
        if(s.mode=="charge"?b+2<a:b-2>a)stable=false;
    }
    double q=0,e=0,covered=0,energy=0;
    if(havePrior&&step.chargeValid&&prior.mode==t["mode"].toString()){
        const double a=qAbs(prior.value("current")),b=qAbs(s.value("current"));
        covered=step.seconds*fraction;q=(a+(b-a)*fraction/2)*covered/3600;
        if(step.energyValid){
            const double pa=a*prior.value("voltage"),pb=b*s.value("voltage");
            energy=covered;e=(pa+(pb-pa)*fraction/2)*covered/3600000;
        }
    }
    QVariant endSoc=s.values.value("soc");
    if(fraction<1){
        endSoc=havePrior&&prior.has("soc")&&s.has("soc")&&step.breakReason.isEmpty()?
            QVariant(prior.value("soc")+(s.value("soc")-prior.value("soc"))*fraction):QVariant();
    }
    if(!execute("INSERT INTO test_samples VALUES(?,?)",QVariantList()<<testId<<sampleId))return false;
    return execute("UPDATE capacity_tests SET last_ms=?,last_mono=?,elapsed_s=?,samples=samples+1,"
        "discharge_mah=discharge_mah+?,charge_mah=charge_mah+?,discharge_mwh=discharge_mwh+?,charge_mwh=charge_mwh+?,"
        "integrated_s=integrated_s+?,energy_s=energy_s+?,end_soc=?,stable_soc=?,gaps=gaps+? WHERE id=?",
        QVariantList()<<s.utc-(s.mono-endpoint)<<endpoint<<elapsed
        <<(s.mode=="discharge"?q:0)<<(s.mode=="charge"?q:0)<<(s.mode=="discharge"?e:0)<<(s.mode=="charge"?e:0)
        <<covered<<energy<<endSoc<<int(stable)<<int(havePrior&&(!step.breakReason.isEmpty()||!step.chargeValid))<<testId);
}
bool Store::finishTest(const QString &reason,bool completed,const QString &request,const Sample *last){
    if(testId.isEmpty())return true;
    Store saved=*this;saved.db=0;
    if(!execute("SAVEPOINT finish_test"))return false;
    bool ok=!last||append(*last);
    QVariantList rows=query("SELECT * FROM capacity_tests WHERE id=?",QVariantList()<<testId);
    if(rows.isEmpty())ok=false;
    QVariantMap t=rows.value(0).toMap();QVariant estimate;
    const double span=t["mode"]=="charge"?t["end_soc"].toDouble()-t["start_soc"].toDouble():t["start_soc"].toDouble()-t["end_soc"].toDouble();
    const double q=t["mode"]=="charge"?t["charge_mah"].toDouble():t["discharge_mah"].toDouble();
    const double elapsed=t["elapsed_s"].toDouble(),coverage=elapsed>0?t["integrated_s"].toDouble()/elapsed:0;
    if(completed&&t["samples"].toInt()>=2&&t["stable_soc"].toBool()&&!t["start_soc"].isNull()&&!t["end_soc"].isNull()
        &&span>=30&&coverage>=0.95&&coverage<=1.001&&q>0&&q*100/span>=1&&q*100/span<=20000)estimate=q*100/span;
    if(ok)ok=execute("UPDATE capacity_tests SET end_ms=last_ms,status=?,reason=?,capacity=? WHERE id=?",
        QVariantList()<<(completed?"completed":"interrupted")<<reason<<estimate<<testId);
    if(ok)ok=execute("UPDATE sessions SET end_ms=last_ms,reason=? WHERE id=?",QVariantList()<<reason<<session);
    if(ok&&!request.isEmpty())ok=execute("INSERT OR REPLACE INTO test_control VALUES(1,?,1,?)",QVariantList()<<request<<QString::fromUtf8("测试已结束并保存"));
    if(ok)ok=execute("RELEASE finish_test");
    if(!ok){QString error=failure;execute("ROLLBACK TO finish_test");execute("RELEASE finish_test");sqlite3 *connection=db;*this=saved;db=connection;failure=error;return false;}
    testId.clear();session=0;havePrevious=false;return true;
}
}
