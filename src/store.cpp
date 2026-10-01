#include "store.h"
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QUuid>
#include <unistd.h>
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
    stableSoc(true),mah(0),mwh(0),covered(0),energyCovered(0),startMono(0){}
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
    int flags=writer?(SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE):SQLITE_OPEN_READONLY;
    if(sqlite3_open_v2(QFile::encodeName(path).constData(),&db,flags,0)!=SQLITE_OK){
        failure=db?QString::fromUtf8(sqlite3_errmsg(db)):"Database open failed";if(db)sqlite3_close(db);db=0;return false;
    }
    sqlite3_busy_timeout(db,writer?2000:500);
    if(!writer) return execute("PRAGMA query_only=ON");
    // DELETE journaling supports read-only clients on this legacy SQLite runtime.
    const char *schema=
        "PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL; PRAGMA foreign_keys=ON;"
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
        "CREATE TABLE IF NOT EXISTS events(id INTEGER PRIMARY KEY,utc_ms INTEGER NOT NULL,kind TEXT NOT NULL,"
        "text TEXT NOT NULL,session_id INTEGER,sample_id INTEGER);"
        "CREATE INDEX IF NOT EXISTS events_time ON events(utc_ms);"
        "CREATE TABLE IF NOT EXISTS collector_state(id INTEGER PRIMARY KEY CHECK(id=1),run_id TEXT,utc_ms INTEGER,"
        "mono_ms INTEGER,interval_s INTEGER,paused INTEGER,error TEXT,pid INTEGER,euid INTEGER);";
    char *err=0;
    if(sqlite3_exec(db,schema,0,0,&err)!=SQLITE_OK){failure=QString::fromUtf8(err?err:"Schema failed");sqlite3_free(err);return false;}
    QVariantList version=query("SELECT value FROM metadata WHERE key='schema'");
    if(version.isEmpty() || version.first().toMap()["value"]!="1"){failure="Unsupported database schema";return false;}
    if(!query("SELECT id FROM samples LIMIT 1").isEmpty())pendingBreak="collector-restart";
    return execute("UPDATE sessions SET end_ms=last_ms,reason='collector-restart' WHERE end_ms IS NULL");
}
bool Store::beginSession(const Sample &s){
    startSoc=s.has("soc")?int(s.value("soc")):-1;lastSoc=startSoc;count=0;mah=0;mwh=0;
    covered=0;energyCovered=0;stableSoc=true;startMono=s.mono;
    QVariant soc=startSoc>=0?QVariant(startSoc):QVariant();
    if(!execute("INSERT INTO sessions(start_ms,last_ms,mode,battery_id,start_soc,end_soc) VALUES(?,?,?,?,?,?)",
       QVariantList()<<s.utc<<s.utc<<s.mode<<s.batteryId<<soc<<soc))return false;
    session=sqlite3_last_insert_rowid(db);return true;
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
    bool ok=execute("BEGIN IMMEDIATE");
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
        int soc=int(s.value("soc"));if(lastSoc>=0 && s.mode=="discharge" && soc>lastSoc+2)stableSoc=false;
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
    ++count;
    if(ok)ok=execute("UPDATE sessions SET last_ms=?,end_soc=?,sample_count=?,mah=?,mwh=?,covered_s=?,energy_covered_s=?,"
        "elapsed_s=?,stable_soc=? WHERE id=?",QVariantList()<<s.utc<<(lastSoc>=0?QVariant(lastSoc):QVariant())<<count<<mah<<mwh
        <<covered<<energyCovered<<qMax(0.0,(s.mono-startMono)/1000.0)<<int(stableSoc)<<session);
    if(ok&&savedHave&&savedPrevious.mode!=s.mode)ok=recordEvent(s,"mode-"+s.mode,modeLabel(s.mode));
    if(ok&&savedHave&&connected(savedPrevious.charger)!=connected(s.charger))
        ok=recordEvent(s,connected(s.charger)?"power-connected":"power-disconnected",
            QString::fromUtf8(connected(s.charger)?"接入外部电源":"断开外部电源"));
    if(ok)ok=execute("COMMIT");
    if(!ok){
        execute("ROLLBACK");previous=savedPrevious;havePrevious=savedHave;session=savedSession;
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
QVariantList Store::history(qint64 begin,qint64 end,QVariantMap *summary,qint64 sessionId){
    HistorySummary stats(begin,end);QVariantList plot,bucket;
    QVariantList maximum=query("SELECT MAX(id) AS last_id FROM samples");
    qint64 maximumId=maximum.isEmpty()?0:maximum.first().toMap()["last_id"].toLongLong();
    QString scope=sessionId?QString(" AND session_id=%1").arg(sessionId):QString();
    scope+=QString(" AND id<=%1").arg(maximumId);
    QVariantList prior=query("SELECT * FROM samples WHERE utc_ms<?"+scope+" ORDER BY utc_ms DESC,id DESC LIMIT 1",QVariantList()<<begin);
    if(!prior.isEmpty())stats.add(prior.first().toMap());
    qint64 width=qMax(qint64(1),(end-begin)/360),index=-1;bool mixed=false;QVariantMap previousRow;
    qint64 cursorTime=begin,cursorId=-1;bool complete=true;
    // Short read statements let the independent writer commit between pages.
    for(;;){
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
    if(summary){*summary=stats.result();(*summary)["complete"]=complete;}
    return plot;
}
bool Store::heartbeat(const QString &run,int interval,bool paused,const QString &error){
    return execute("INSERT OR REPLACE INTO collector_state VALUES(1,?,?,?,?,?,?,?,?)",
        QVariantList()<<run<<utcMillis()<<monoMillis()<<interval<<int(paused)<<error<<qint64(getpid())<<qint64(geteuid()));
}
}
