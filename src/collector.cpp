#include "store.h"
#include <QCoreApplication>
#include <QFile>
#include <QDir>
#include <QUuid>
#include <QSettings>
#include <bps/bps.h>
#include <bps/battery.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <signal.h>
#include <grp.h>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>

namespace {
volatile sig_atomic_t stopping=0;
void stop(int){stopping=1;}
QString statusSnapshot(){
    int fd=::open("/pps/system/BattMgr/status",O_RDONLY|O_NONBLOCK);
    if(fd<0)return QString();
    char raw[32768];ssize_t n=::read(fd,raw,sizeof(raw));::close(fd);
    return n>0?QString::fromUtf8(raw,int(n)):QString();
}
Battery::Sample capture(const QString &run,int interval){
    Battery::Sample s;s.utc=Battery::utcMillis();s.mono=Battery::monoMillis();s.run=run;s.interval=interval;
    s.raw=statusSnapshot();battery_info_t *info=0;
    if(battery_get_info(&info)!=BPS_SUCCESS || !info){
        s.error=QString("battery_get_info failed: %1").arg(errno);s.mode="unknown";return s;
    }
    s.ready=battery_info_is_battery_ready(info)&&battery_info_is_battery_present(info);
    s.batteryId=battery_info_get_battery_id(info);
    if(s.ready){
        s.values["soc"]=Battery::number(battery_info_get_state_of_charge(info),0,100);
        s.values["current"]=Battery::number(battery_info_get_battery_average_current(info),-10000,10000);
        s.values["voltage"]=Battery::number(battery_info_get_battery_voltage(info),2000,5000);
        s.values["temperature"]=Battery::number(battery_info_get_battery_temperature(info),-40,100);
        s.values["design"]=Battery::number(battery_info_get_battery_design_capacity(info),1,20000);
        s.values["full_capacity"]=Battery::number(battery_info_get_battery_full_charge_capacity(info),1,20000);
        s.values["remaining"]=Battery::number(battery_info_get_battery_nominal_available_capacity(info),0,20000);
        s.values["health"]=Battery::number(battery_info_get_state_of_health(info),0,100);
        s.values["cycles"]=Battery::number(battery_info_get_battery_cycle_count(info),0,65535);
    }
    const char *charger[]={"ERROR","BAD","NONE","PLUGGED","CHARGING"};
    int c=battery_info_get_charger_info(info);s.charger=c>=0&&c<5?charger[c]:"UNKNOWN";
    if(s.charger=="UNKNOWN"){
        QString fallback=Battery::sourceDetails(s.raw).value("charger").toString();
        if(!fallback.isEmpty())s.charger=fallback;
    }
    const char *phase[]={"NC","TRICKLE","CC","CV","DONE"};
    int p=battery_info_get_system_charging_state(info);s.phase=p>=0&&p<5?phase[p]:"UNKNOWN";
    if(battery_info_is_charger_ready(info)){
        s.values["input_limit"]=Battery::number(battery_info_get_charger_max_input_current(info),0,10000);
        s.values["charge_limit"]=Battery::number(battery_info_get_charger_max_charge_current(info),0,10000);
    }
    if(battery_info_is_system_ready(info))
        s.values["input_current"]=Battery::number(battery_info_get_system_input_current_monitor(info),0,10000);
    if(s.raw.isEmpty())s.error="Raw PPS snapshot unavailable";
    s.mode=Battery::modeFor(s);battery_free_info(&info);return s;
}
bool storageTests(QString *report){
    QString path=QString("/tmp/bbattery-test-%1.sqlite").arg(getpid());
    bool ok=true;
    {
        Battery::Store db;ok=db.open(path,true);
        Battery::Sample s;s.utc=100000;s.mono=100000;s.run="test";s.ready=true;s.batteryId=244;s.interval=10;
        s.mode="discharge";s.raw="snapshot";s.values["soc"]=90;s.values["current"]=-360;s.values["voltage"]=4000;
        ok=ok&&db.append(s);
        s.utc+=10000;s.mono+=10000;ok=ok&&db.append(s);
        QVariantList rows=db.query("SELECT * FROM sessions");ok=ok&&rows.size()==1&&rows.first().toMap()["sample_count"].toInt()==2;
        ok=ok&&qAbs(rows.first().toMap()["mah"].toDouble()-1.0)<0.0001;
        ok=ok&&db.query("SELECT id FROM snapshots").size()==1;
        ok=ok&&db.query("SELECT full_capacity FROM samples WHERE full_capacity IS NOT NULL").isEmpty();
        s.utc+=60000;s.mono+=60000;ok=ok&&db.append(s);
        ok=ok&&db.query("SELECT id FROM sessions").size()==2;
        ok=ok&&db.query("SELECT id FROM samples WHERE gap_reason='sampling-gap'").size()==1;
        ok=ok&&db.closeSession("test-paused");
        s.utc+=10000;s.mono+=10000;ok=ok&&db.append(s);
        ok=ok&&db.query("SELECT id FROM sessions").size()==3;
        if(!ok)*report+="FAIL: initial storage tests: "+db.error()+"\n";
        s.charger="NONE";s.mode="discharge";s.utc+=10000;s.mono+=10000;ok=ok&&db.append(s);
        s.charger="PLUGGED";s.mode="charge";s.values["current"]=360;s.utc+=10000;s.mono+=10000;ok=ok&&db.append(s);
        ok=ok&&db.query("SELECT id FROM events WHERE kind='power-connected'").size()==1;
        ok=ok&&db.query("SELECT id FROM events WHERE kind='mode-charge'").size()==1;
        if(!ok)*report+="FAIL: transition event storage: "+db.error()+"\n";
        ok=ok&&db.recordEvent(s,"test-alert","test",600)&&db.recordEvent(s,"test-alert","test",600);
        ok=ok&&db.query("SELECT id FROM events WHERE kind='test-alert'").size()==1;
        if(!ok)*report+="FAIL: alert cooldown storage: "+db.error()+"\n";
        QVariantMap stats;QVariantList plot=db.history(100000,200000,&stats);
        ok=ok&&stats["samples"].toInt()==6&&plot.size()<=6;
        if(!ok)*report+=QString("FAIL: history stream samples=%1 plot=%2: %3\n").arg(stats["samples"].toInt()).arg(plot.size()).arg(db.error());
        ok=ok&&db.heartbeat("test",10,false,"");
        Battery::Store reader;ok=ok&&reader.open(path,false)&&reader.query("SELECT id FROM samples").size()==6;
        ok=ok&&!reader.execute("DELETE FROM samples");
        if(ok){
            const int n=25010;sqlite3_stmt *insert=0;
            ok=db.execute("BEGIN IMMEDIATE")&&sqlite3_prepare_v2(db.handle(),
                "INSERT INTO samples(utc_ms,mono_ms,run_id,session_id,interval_s,ready,battery_id,mode,charger,soc,current,"
                "voltage,temperature,gap_reason) VALUES(?,?,'test',4,10,1,244,'charge','PLUGGED',90,?,4000,25,'')",
                -1,&insert,0)==SQLITE_OK;
            for(int i=0;ok&&i<n;++i){
                sqlite3_bind_int64(insert,1,s.utc+(i+1)*qint64(10000));sqlite3_bind_int64(insert,2,s.mono+(i+1)*qint64(10000));
                sqlite3_bind_double(insert,3,100+i%100);ok=sqlite3_step(insert)==SQLITE_DONE;sqlite3_reset(insert);
            }
            if(insert)sqlite3_finalize(insert);
            ok=ok&&db.execute("COMMIT");
            plot=db.history(s.utc+10000,s.utc+n*qint64(10000),&stats);
            ok=ok&&stats["samples"].toInt()==n&&plot.size()<4000&&stats["currentMax"].toDouble()==199;
            *report+=ok?"PASS: full 25010-row history statistics with bounded extrema-preserving rendering\n":
                "FAIL: complete long-history statistics\n";
        }
    }
    QFile::remove(path);QFile::remove(path+"-journal");
    *report+=ok?"PASS: SQLite transactions, deduplicated raw snapshots, NULL preservation, gap and pause boundaries, read-only GUI\n":
        "FAIL: SQLite storage tests\n";return ok;
}
}
int main(int argc,char **argv){
    QCoreApplication app(argc,argv);QStringList args=app.arguments();
    if(args.contains("--self-test")){
        QString report;bool ok=Battery::selfTest(&report);ok=storageTests(&report)&&ok;
        std::fputs(report.toUtf8().constData(),stdout);return ok?0:1;
    }
    int index=args.indexOf("--data");if(index<0 || index+1>=args.size()){std::fputs("--data is required\n",stderr);return 2;}
    QString dir=args[index+1];
    if(!dir.startsWith("/accounts/1000/appdata/top.blaccat.BBattery.") || !dir.endsWith("/data/battery")){
        std::fputs("Unexpected application data directory\n",stderr);return 2;
    }
    int uidIndex=args.indexOf("--uid"),gidIndex=args.indexOf("--gid");
    if(geteuid()==0 && uidIndex>=0 && gidIndex>=0 && uidIndex+1<args.size() && gidIndex+1<args.size()){
        bool u=false,g=false;int uid=args[uidIndex+1].toInt(&u),gid=args[gidIndex+1].toInt(&g);
        // Only the account traversal group is needed in addition to this app's GID.
        const gid_t accountGroup=1000;
        if(!u||!g||uid<=0||gid<=0||setgroups(1,&accountGroup)!=0||setgid(gid)!=0||setuid(uid)!=0||geteuid()!=uid){
            std::fputs("Cannot narrow collector identity\n",stderr);return 3;
        }
    }
    if(geteuid()==0){std::fputs("Collector refuses persistent UID 0\n",stderr);return 3;}
    umask(0077);QDir().mkpath(dir);
    int lock=::open(QFile::encodeName(dir+"/collector.lock").constData(),O_CREAT|O_RDWR,0600);
    if(lock<0 || flock(lock,LOCK_EX|LOCK_NB)!=0){
        std::fprintf(stderr,"Collector lock unavailable: %s\n",std::strerror(errno));return 4;
    }
    QFile pid(dir+"/collector.pid");if(pid.open(QIODevice::WriteOnly)){pid.write(QByteArray::number(getpid()));pid.close();}
    signal(SIGTERM,stop);signal(SIGINT,stop);signal(SIGHUP,SIG_IGN);
    Battery::Store db;if(!db.open(dir+"/history.sqlite",true)){std::fputs(db.error().toUtf8().constData(),stderr);return 5;}
    if(bps_initialize()!=BPS_SUCCESS){std::fputs("BPS initialization failed\n",stderr);return 6;}
    battery_request_events(0);
    const QString run=QUuid::createUuid().toString();qint64 next=0,lastHeartbeat=0,lastSample=0;
    bool wasPaused=false,eventPending=false;QString failure;Battery::AlertMonitor alerts;
    std::fprintf(stderr,"BBattery collector 0.1.0.5 started; pid=%d euid=%d\n",int(getpid()),int(geteuid()));
    while(!stopping){
        QSettings settings(dir+"/settings.ini",QSettings::IniFormat);
        int interval=settings.value("interval",10).toInt();if(interval!=5&&interval!=10&&interval!=30&&interval!=60)interval=10;
        bool paused=settings.value("paused",false).toBool();
        if(paused&&!wasPaused){if(!db.closeSession("paused"))failure=db.error();next=0;}
        if(!paused&&wasPaused)next=0;
        wasPaused=paused;qint64 now=Battery::monoMillis();
        if(!paused&&(now>=next||(eventPending&&now-lastSample>=2000))){
            Battery::Sample s=capture(run,interval);
            if(db.append(s)){
                failure=s.error;Battery::AlertConfig config;
                config.enabled=settings.value("alerts/enabled",true).toBool();
                config.lowSoc=qBound(5,settings.value("alerts/low",20).toInt(),40);
                config.highSoc=qBound(60,settings.value("alerts/high",90).toInt(),100);
                config.temperature=qBound(35,settings.value("alerts/temperature",45).toInt(),55);
                QStringList triggered=alerts.evaluate(s,config);
                foreach(const QString &kind,triggered){
                    QString text=kind=="low-soc"?QString::fromUtf8("低电量 · %1%").arg(s.value("soc"),0,'f',0):
                        kind=="high-soc"?QString::fromUtf8("充电达到提醒电量 · %1%").arg(s.value("soc"),0,'f',0):
                        QString::fromUtf8("电池温度偏高 · %1 C").arg(s.value("temperature"),0,'f',1);
                    if(!db.recordEvent(s,kind,text,600))failure=db.error();
                }
            }else failure=db.error();
            lastSample=now;eventPending=false;
            if(now>=next)next=now+qint64(interval)*1000;
        }
        if(now-lastHeartbeat>=5000){
            if(!db.heartbeat(run,interval,paused,failure))std::fputs("Database heartbeat failed\n",stderr);
            lastHeartbeat=now;
        }
        bps_event_t *event=0;
        qint64 due=eventPending?qMin(next,lastSample+2000):next;
        int wait=paused?5000:int(qMin(qint64(5000),qMax(qint64(1),due-Battery::monoMillis())));
        bps_get_event(&event,wait);
        while(event){
            if(bps_event_get_domain(event)==battery_get_domain()&&bps_event_get_code(event)==BATTERY_INFO)eventPending=true;
            if(bps_get_event(&event,0)!=BPS_SUCCESS)break;
        }
    }
    db.closeSession("collector-stopped");db.heartbeat(run,10,true,"Collector stopped");
    battery_stop_events(0);bps_shutdown();::close(lock);return 0;
}
