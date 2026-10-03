#include "store.h"
#include "collector_state.h"
#include <QCoreApplication>
#include <QFile>
#include <QDir>
#include <QUuid>
#include <QSettings>
#include <QCryptographicHash>
#include <bps/bps.h>
#include <bps/battery.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/wait.h>
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
    QString path=QDir::tempPath()+QString("/bbattery-test-%1.sqlite").arg(getpid());
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
bool batteryTests(QString *report){
    const QString path=QDir::tempPath()+QString("/bbattery-profile-test-%1.sqlite").arg(getpid());bool ok=true;
    QVariantList preserved;
    {
        Battery::Store db;ok=db.open(path,true);
        Battery::Sample s;s.utc=100000;s.mono=100000;s.run="profile-test";s.ready=true;
        s.batteryId=244;s.interval=10;s.mode="discharge";s.raw="profile-snapshot";
        s.values["soc"]=90;s.values["current"]=-3600;s.values["voltage"]=4000;
        ok=ok&&db.append(s);s.utc+=10000;s.mono+=10000;s.values["soc"]=30;ok=ok&&db.append(s);
        preserved=db.query("SELECT * FROM samples ORDER BY id");
        ok=ok&&db.setBattery("spare",QString::fromUtf8("备用 A"));
        s.utc+=10000;s.mono+=10000;s.values["soc"]=90;ok=ok&&db.append(s);
        QVariantMap firstSpare=db.query("SELECT * FROM sessions ORDER BY id DESC LIMIT 1").value(0).toMap();
        ok=ok&&firstSpare["mah"].toDouble()==0&&firstSpare["sample_count"].toInt()==1;
        s.utc+=10000;s.mono+=10000;s.values["soc"]=30;ok=ok&&db.append(s);
        ok=ok&&db.setBattery("legacy",QString::fromUtf8("原装电池"));
        s.utc+=10000;s.mono+=10000;s.values["soc"]=80;ok=ok&&db.append(s);
        ok=ok&&db.capacitySession("discharge",244,0,"legacy")["id"].toLongLong()==1;
        ok=ok&&db.capacitySession("discharge",244,0,"spare")["id"].toLongLong()==2;
        ok=ok&&db.query("SELECT * FROM samples WHERE id<=2 ORDER BY id")==preserved;
        ok=ok&&db.registerBattery("legacy",QString::fromUtf8("原装重命名"));
        ok=ok&&db.query("SELECT id FROM sessions").size()==3;
        QVariantMap stats;db.history(100000,150000,&stats,0,"legacy");
        ok=ok&&stats["samples"].toInt()==3&&qAbs(stats["dischargeMah"].toDouble()-10)<0.0001;
        db.history(100000,150000,&stats,0,"spare");ok=ok&&stats["samples"].toInt()==2;
        ok=ok&&db.setBattery("spare",QString::fromUtf8("备用 A"));
        ok=ok&&db.execute("CREATE TEMP TRIGGER reject_mapping BEFORE INSERT ON session_batteries BEGIN SELECT RAISE(ABORT,'test rollback'); END");
        s.utc+=10000;s.mono+=10000;ok=ok&&!db.append(s);
        ok=ok&&db.query("SELECT id FROM samples").size()==5&&db.query("SELECT id FROM sessions").size()==3;
        ok=ok&&db.execute("DROP TRIGGER reject_mapping")&&db.append(s);
        ok=ok&&db.query("SELECT battery_key FROM session_batteries WHERE session_id=4").value(0).toMap()["battery_key"]=="spare";
        if(!ok)*report+="FAIL: battery profiles: "+db.error()+"\n";
    }
    {
        Battery::Store db;ok=ok&&db.open(path,true);
        ok=ok&&db.query("SELECT * FROM samples WHERE id<=2 ORDER BY id")==preserved;
        ok=ok&&db.query("SELECT session_id FROM session_batteries WHERE battery_key='spare'").size()==2;
        ok=ok&&db.capacitySession("discharge",244,0,"legacy")["id"].toLongLong()==1;
        Battery::Sample s;s.utc=200000;s.mono=200000;s.run="legacy-soc";s.ready=true;
        s.batteryId=244;s.interval=10;s.mode="charge";s.raw="legacy-soc-snapshot";
        s.values["current"]=3600;s.values["soc"]=20;
        ok=ok&&db.append(s);s.utc+=10000;s.mono+=10000;s.values["soc"]=50;ok=ok&&db.append(s);
        s.utc+=10000;s.mono+=10000;s.values["soc"]=80;ok=ok&&db.append(s)&&db.closeSession("test-completed");
        const int readings[]={20,60,40,80};
        for(int i=0;i<4;++i){s.utc+=10000;s.mono+=10000;s.values["soc"]=readings[i];ok=ok&&db.append(s);}
        ok=ok&&db.closeSession("test-completed")&&db.execute("UPDATE sessions SET stable_soc=1 WHERE id=6");
        ok=ok&&db.capacitySession("charge",244,0,"legacy")["id"].toLongLong()==5;
        ok=ok&&db.capacitySession("charge",244,6,"legacy").isEmpty();
        if(!ok)*report+="FAIL: paged legacy SOC validation: "+db.error()+"\n";
    }
    QFile::remove(path);QFile::remove(path+"-journal");
    *report+=ok?"PASS: battery profile A/B/A identity, isolated integration and capacity, rename, rollback, restart and sample preservation\n":
        "FAIL: battery profile tests\n";return ok;
}
bool testCheck(bool condition,const char *label,QString *report){
    *report+=QString(condition?"PASS: ":"FAIL: ")+label+"\n";return condition;
}
bool intervalTests(QString *report){
    const QString path=QDir::tempPath()+QString("/bbattery-interval-test-%1.sqlite").arg(getpid());bool ok=true;
    {
        Battery::Store db;ok=db.open(path,true);
        Battery::Sample s;s.utc=100000;s.mono=100000;s.run="interval-test";s.ready=true;s.batteryId=244;s.interval=30;
        s.mode="discharge";s.raw="interval-snapshot";s.values["soc"]=90.2;s.values["current"]=-3600;s.values["voltage"]=4000;
        ok=testCheck(db.startTest("timer","legacy","A",60,s),"test starts with immediate sample",report)&&ok;
        ok=testCheck(!db.startTest("duplicate","legacy","A",60,s),"duplicate start rejected",report)&&ok;
        s.utc+=30000;s.mono+=30000;s.values["soc"]=75.2;ok=db.append(s)&&ok;
        s.utc+=31000;s.mono+=31000;s.values["soc"]=59.7;ok=db.append(s)&&db.finishTest("timer",true)&&ok;
        QVariantMap t=db.query("SELECT * FROM capacity_tests WHERE id='timer'").value(0).toMap();
        ok=testCheck(qAbs(t["discharge_mah"].toDouble()-60)<0.0001&&qAbs(t["discharge_mwh"].toDouble()-240)<0.0001
            &&t["elapsed_s"].toDouble()==60&&qAbs(t["end_soc"].toDouble()-60.2)<0.0001,
            "deadline clips charge, energy, SOC and elapsed time",report)&&ok;
        ok=testCheck(qAbs(t["capacity"].toDouble()-200)<0.0001&&t["status"]=="completed"&&db.runningTest().isEmpty(),
            "completed fractional-SOC capacity with 100 percent coverage",report)&&ok;
        ok=testCheck(db.query("SELECT * FROM test_samples WHERE test_id='timer'").size()==3,"exact sample ownership",report)&&ok;
        const QVariantList preserved=db.query("SELECT * FROM samples ORDER BY id");
        s.utc+=30000;s.mono+=30000;s.values["soc"]=80;
        ok=db.startTest("manual","spare","B",60,s)&&ok;
        s.utc+=15000;s.mono+=15000;s.values["soc"]=75;ok=db.append(s)&&db.finishTest("manual",true)&&ok;
        t=db.query("SELECT * FROM capacity_tests WHERE id='manual'").value(0).toMap();
        ok=testCheck(t["elapsed_s"].toDouble()==15&&qAbs(t["discharge_mah"].toDouble()-15)<0.0001&&t["capacity"].isNull(),
            "manual finish and short SOC range refuses extrapolation",report)&&ok;
        ok=testCheck(db.query("SELECT * FROM samples WHERE id<=3 ORDER BY id")==preserved&&t["battery_key"]=="spare",
            "battery switch preserves prior samples and isolates tests",report)&&ok;
        ok=db.registerBattery("spare","renamed B")&&ok;
        ok=testCheck(db.query("SELECT id FROM capacity_tests WHERE battery_key='spare'").size()==1,"rename preserves test identity",report)&&ok;
        s.utc+=30000;s.mono+=30000;s.values["soc"]=90;ok=db.startTest("gap","legacy","A",180,s)&&ok;
        s.utc+=120000;s.mono+=120000;s.values["soc"]=30;ok=db.append(s)&&db.finishTest("manual",true)&&ok;
        t=db.query("SELECT * FROM capacity_tests WHERE id='gap'").value(0).toMap();
        ok=testCheck(t["integrated_s"].toDouble()==0&&t["gaps"].toInt()==1&&t["capacity"].isNull(),"sampling gap is excluded",report)&&ok;
        s.utc+=30000;s.mono+=30000;s.values["soc"]=90;ok=db.startTest("mixed","legacy","A",60,s)&&ok;
        s.utc+=30000;s.mono+=30000;s.mode="charge";s.values["current"]=3600;s.values["soc"]=30;
        ok=db.append(s)&&db.finishTest("state-changed",false)&&ok;
        t=db.query("SELECT * FROM capacity_tests WHERE id='mixed'").value(0).toMap();
        ok=testCheck(t["integrated_s"].toDouble()==0&&t["status"]=="interrupted"&&t["capacity"].isNull(),"mixed direction refuses capacity",report)&&ok;
        s.utc+=30000;s.mono+=30000;s.mode="charge";s.values["soc"]=20;ok=db.startTest("unstable","legacy","A",90,s)&&ok;
        s.utc+=30000;s.mono+=30000;s.values["soc"]=60;ok=db.append(s)&&ok;
        s.utc+=30000;s.mono+=30000;s.values["soc"]=40;ok=db.append(s)&&ok;
        s.utc+=30000;s.mono+=30000;s.values["soc"]=80;ok=db.append(s)&&db.finishTest("timer",true)&&ok;
        t=db.query("SELECT * FROM capacity_tests WHERE id='unstable'").value(0).toMap();
        ok=testCheck(!t["stable_soc"].toBool()&&t["capacity"].isNull(),"interior SOC reversal refuses capacity",report)&&ok;
        s.utc+=30000;s.mono+=30000;s.values.remove("soc");s.values.remove("voltage");
        ok=db.startTest("missing","legacy","A",60,s)&&ok;
        s.utc+=30000;s.mono+=30000;ok=db.append(s)&&db.finishTest("manual",true)&&ok;
        t=db.query("SELECT * FROM capacity_tests WHERE id='missing'").value(0).toMap();
        ok=testCheck(t["energy_s"].toDouble()==0&&t["capacity"].isNull()&&t["start_soc"].isNull(),"missing SOC and voltage remain unavailable",report)&&ok;
        s.utc+=30000;s.mono+=30000;s.values["soc"]=20;s.values["voltage"]=4000;
        ok=db.startTest("rollback","legacy","A",60,s)&&ok;
        const int before=db.query("SELECT id FROM samples").size();
        ok=db.execute("CREATE TEMP TRIGGER reject_test BEFORE UPDATE ON capacity_tests BEGIN SELECT RAISE(ABORT,'test rollback'); END")&&ok;
        s.utc+=30000;s.mono+=30000;s.values["soc"]=50;
        ok=testCheck(!db.append(s)&&db.query("SELECT id FROM samples").size()==before&&
            db.query("SELECT sample_id FROM test_samples WHERE test_id='rollback'").size()==1,"test update and sample transaction roll back together",report)&&ok;
        ok=db.execute("DROP TRIGGER reject_test")&&db.append(s)&&db.finishTest("manual",true)&&ok;
        s.utc+=30000;s.mono+=30000;s.mode="plugged";
        ok=testCheck(!db.startTest("invalid","legacy","A",60,s),"idle charge state cannot start capacity test",report)&&ok;
        s.mode="charge";ok=db.startTest("restart","legacy","A",60,s)&&ok;
    }
    {
        Battery::Store db;ok=db.open(path,true)&&ok;
        QVariantMap t=db.query("SELECT * FROM capacity_tests WHERE id='restart'").value(0).toMap();
        ok=testCheck(t["status"]=="interrupted"&&t["reason"]=="collector-restart"&&t["capacity"].isNull()&&db.runningTest().isEmpty(),
            "restart persists and interrupts test without resuming",report)&&ok;
        ok=testCheck(db.query("SELECT id FROM capacity_tests WHERE battery_key='spare'").size()==1,"battery identity survives restart",report)&&ok;
    }
    QFile::remove(path);QFile::remove(path+"-journal");
    *report+=ok?"PASS: interval test storage and measurement boundaries\n":"FAIL: interval test storage and measurement boundaries\n";
    return ok;
}
bool durabilityTests(QString *report){
    const QString path=QDir::tempPath()+QString("/bbattery-durable-%1.sqlite").arg(getpid());bool ok=true;
    {
        Battery::Store db;ok=db.open(path,true);
        Battery::Sample s;s.utc=100000;s.mono=100000;s.run="durability";s.ready=true;s.batteryId=244;s.interval=10;
        s.mode="discharge";s.raw="raw";s.values["soc"]=90;s.values["current"]=-360;s.values["voltage"]=4000;
        ok=db.execute("CREATE TEMP TRIGGER reject_ack BEFORE INSERT ON test_control BEGIN SELECT RAISE(ABORT,'ack fault'); END")&&ok;
        ok=testCheck(!db.startTest("original","legacy","A",60,s)&&db.runningTest().isEmpty()
            &&db.query("SELECT id FROM capacity_tests").isEmpty()&&db.query("SELECT id FROM samples").isEmpty(),
            "start acknowledgement failure rolls back test, sample and memory",report)&&ok;
        ok=db.execute("DROP TRIGGER reject_ack")&&db.startTest("original","legacy","A",60,s)&&ok;
        ok=testCheck(!db.startTest("original","legacy","A",60,s)&&db.query("SELECT id FROM samples").size()==1
            &&db.query("SELECT id FROM capacity_tests").size()==1,"accepted request cannot create a duplicate test",report)&&ok;
        QVariantList before=db.query("SELECT * FROM capacity_tests");s.utc+=10000;s.mono+=10000;
        ok=db.execute("CREATE TEMP TRIGGER reject_ack BEFORE INSERT ON test_control BEGIN SELECT RAISE(ABORT,'ack fault'); END")&&ok;
        ok=testCheck(!db.finishTest("manual",true,"stop-original",&s)&&db.runningTest()=="original"
            &&db.query("SELECT * FROM capacity_tests")==before&&db.query("SELECT id FROM samples").size()==1,
            "stop acknowledgement failure rolls back endpoint and completion",report)&&ok;
        ok=db.execute("DROP TRIGGER reject_ack")&&db.finishTest("manual",true,"stop-original",&s)&&ok;
        ok=testCheck(db.runningTest().isEmpty()&&db.query("SELECT request_id FROM test_control").value(0).toMap()["request_id"]=="stop-original"
            &&db.query("SELECT id FROM samples").size()==2,"stop result and acknowledgement commit together",report)&&ok;
        ok=db.execute("CREATE TABLE budget_probe(data BLOB)")&&ok;
        qint64 pages=db.query("PRAGMA page_count").value(0).toMap()["page_count"].toLongLong();
        ok=db.execute(QString("PRAGMA max_page_count=%1").arg(pages))&&ok;
        ok=testCheck(!db.execute("INSERT INTO budget_probe VALUES(zeroblob(1048576))")
            &&db.query("SELECT id FROM samples").size()==2&&db.query("PRAGMA quick_check").value(0).toMap().values().contains("ok"),
            "SQLite storage limit rejects growth and preserves completed data",report)&&ok;
    }
    QFile::remove(path);QFile::remove(path+"-journal");return ok;
}
bool readinessTests(QString *report){
    const QString dir=QDir::tempPath()+QString("/bbattery-ready-%1").arg(getpid());QDir().mkpath(dir);
    const QByteArray file=QFile::encodeName(dir+"/collector.lock");
    int lock=::open(file.constData(),O_CREAT|O_RDWR,0600);if(lock<0)return false;::close(lock);
    QVariantMap state;state["pid"]=qint64(getpid());state["run_id"]="isolated";state["euid"]=qint64(geteuid());state["mono_ms"]=Battery::monoMillis();
    bool ok=testCheck(Battery::collectorHealth(dir,state)==Battery::CollectorStopped,"fresh heartbeat without held lock is stopped",report);
    int channel[2];if(pipe(channel)!=0)return false;
    pid_t child=fork();
    if(child==0){
        ::close(channel[0]);int held=::open(file.constData(),O_RDWR);
        bool ready=held>=0&&flock(held,LOCK_EX|LOCK_NB)==0&&Battery::writeCollectorInstance(dir,"isolated");
        char result=ready?'1':'0';::write(channel[1],&result,1);::close(channel[1]);
        if(ready)pause();_exit(ready?0:1);
    }
    ::close(channel[1]);char result='0';if(child>0)::read(channel[0],&result,1);::close(channel[0]);
    if(child>0&&result=='1'){
        state["pid"]=qint64(child);state["mono_ms"]=Battery::monoMillis();
        ok=testCheck(Battery::collectorHealth(dir,state)==Battery::CollectorReady,"held lock, live instance and fresh business heartbeat are ready",report)&&ok;
        state["mono_ms"]=Battery::monoMillis()+60000;
        ok=testCheck(Battery::collectorHealth(dir,state)!=Battery::CollectorReady,"future monotonic heartbeat is rejected",report)&&ok;
        state["mono_ms"]=Battery::monoMillis()-60000;
        ok=testCheck(Battery::collectorHealth(dir,state)!=Battery::CollectorReady,"stale heartbeat is rejected",report)&&ok;
        state["mono_ms"]=Battery::monoMillis();state["pid"]=qint64(getpid());
        ok=testCheck(Battery::collectorHealth(dir,state)!=Battery::CollectorReady,"unrelated live PID is rejected",report)&&ok;
        lock=::open(file.constData(),O_RDWR);
        ok=testCheck(lock>=0&&flock(lock,LOCK_EX|LOCK_NB)!=0,"second process cannot take collector lock",report)&&ok;
        if(lock>=0)::close(lock);
    }else ok=false;
    if(child>0){kill(child,SIGTERM);waitpid(child,0,0);}
    ok=testCheck(Battery::collectorHealth(dir,state)==Battery::CollectorStopped,"crashed instance releases persistent lock without deletion",report)&&ok;
    QFile::remove(dir+"/collector.instance");QFile::remove(dir+"/collector.lock");QDir().rmdir(dir);return ok;
}
}
int main(int argc,char **argv){
    QCoreApplication app(argc,argv);QStringList args=app.arguments();
    if(args.contains("--self-test")){
        QString report;bool ok=Battery::selfTest(&report);ok=storageTests(&report)&&ok;ok=batteryTests(&report)&&ok;ok=intervalTests(&report)&&ok;
        ok=durabilityTests(&report)&&ok;ok=readinessTests(&report)&&ok;
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
    if(args.contains("--status")){
        Battery::Store reader;QVariantMap state;bool readable=reader.open(dir+"/history.sqlite",false);
        if(readable)state=reader.query("SELECT * FROM collector_state WHERE id=1").value(0).toMap();
        Battery::CollectorHealth health=Battery::collectorHealth(dir,state);
        if(!readable&&QFile::exists(dir+"/history.sqlite"))health=Battery::CollectorUnknown;
        const bool running=readable&&!reader.query("SELECT id FROM capacity_tests WHERE status='running' LIMIT 1").isEmpty();
        QSettings intent(dir+"/settings.ini",QSettings::IniFormat);
        const QString request=intent.value("test/request").toString();
        const QString handled=readable?reader.query("SELECT request_id FROM test_control WHERE id=1").value(0).toMap()["request_id"].toString():QString();
        std::printf("health=%d\npid=%lld\nrunning=%d\npending=%d\n",int(health),state.value("pid").toLongLong(),int(running),int(!request.isEmpty()&&request!=handled));
        return int(health);
    }
    umask(0077);QDir().mkpath(dir);
    int lock=Battery::openCollectorLock(dir,true);
    if(lock<0 || flock(lock,LOCK_EX|LOCK_NB)!=0){
        std::fprintf(stderr,"Collector lock unavailable: %s\n",std::strerror(errno));return 4;
    }
    QFile pid(dir+"/collector.pid");if(pid.open(QIODevice::WriteOnly)){pid.write(QByteArray::number(getpid()));pid.close();}
    signal(SIGTERM,stop);signal(SIGINT,stop);signal(SIGHUP,SIG_IGN);signal(SIGPIPE,SIG_IGN);
    Battery::Store db;if(!db.open(dir+"/history.sqlite",true)){std::fputs(db.error().toUtf8().constData(),stderr);return 5;}
    if(bps_initialize()!=BPS_SUCCESS){std::fputs("BPS initialization failed\n",stderr);return 6;}
    const QString run=QUuid::createUuid().toString();qint64 next=0,lastHeartbeat=0,deadline=0;
    if(!Battery::writeCollectorInstance(dir,run)){std::fputs("Collector instance persistence failed\n",stderr);return 7;}
    QString failure,activeKey;QMap<QString,QString> labels;
    std::fprintf(stderr,"BBattery collector 0.1.0.9 interval tests; pid=%d euid=%d\n",int(getpid()),int(geteuid()));
    while(!stopping){
        QSettings settings(dir+"/settings.ini",QSettings::IniFormat);
        settings.beginGroup("batteries");QStringList keys=settings.childKeys();settings.endGroup();
        if(!keys.contains("legacy"))keys.prepend("legacy");
        bool batteryReady=true;
        foreach(const QString &key,keys){
            QString label=settings.value("batteries/"+key,QString::fromUtf8("电池 1")).toString().trimmed();
            if(labels.value(key)!=label){
                if(db.registerBattery(key,label))labels[key]=label;else{batteryReady=false;failure=db.error();}
            }
        }
        QString key=settings.value("battery/active","legacy").toString();if(!keys.contains(key))key="legacy";
        if(key!=activeKey&&batteryReady){
            if(!db.runningTest().isEmpty()&&!db.finishTest("battery-marker-changed",false))batteryReady=false;
            if(batteryReady&&db.setBattery(key,labels.value(key)))activeKey=key;
            else{batteryReady=false;failure=db.error();}
        }
        int interval=settings.value("test/interval",30).toInt();if(interval!=10&&interval!=30&&interval!=60)interval=30;
        const QString request=settings.value("test/request").toString();
        const QString handled=db.query("SELECT request_id FROM test_control WHERE id=1").value(0).toMap()["request_id"].toString();
        qint64 now=Battery::monoMillis();
        if(!request.isEmpty()&&request!=handled){
            bool ok=false;QString note;QString operation=settings.value("test/operation").toString();
            const QString parameters=operation+"\n"+QString::number(settings.value("test/seconds").toInt())+"\n"+QString::number(interval)+"\n"+settings.value("test/battery").toString()+"\n"+settings.value("test/target").toString();
            const QString digest=QString::fromLatin1(QCryptographicHash::hash(parameters.toUtf8(),QCryptographicHash::Sha1).toHex());
            const QVariantList priorTest=db.query("SELECT battery_key,requested_s FROM capacity_tests WHERE id=?",QVariantList()<<request);
            if(settings.value("test/digest").toString()!=digest)note=QString::fromUtf8("操作参数校验失败，请重新操作");
            else if(operation=="start"&&!priorTest.isEmpty()){
                ok=priorTest[0].toMap()["battery_key"]==settings.value("test/battery")
                   &&priorTest[0].toMap()["requested_s"].toInt()==settings.value("test/seconds").toInt();
                note=QString::fromUtf8(ok?"原测试已受理，请查看测试结果":"原编号参数不一致，操作已拒绝");
            }
            else if(settings.value("test/expires").toLongLong()<Battery::utcMillis())note=QString::fromUtf8("操作已过期，请重试");
            else if(operation=="start"){
                if(!db.runningTest().isEmpty())note=QString::fromUtf8("已有测试正在进行");
                else if(!batteryReady||settings.value("test/battery").toString()!=activeKey)note=QString::fromUtf8("电池标记尚未就绪");
                else{
                    Battery::Sample s=capture(run,interval);
                    ok=db.startTest(request,activeKey,labels.value(activeKey),settings.value("test/seconds").toInt(),s);
                    note=ok?QString::fromUtf8("测试已开始"):QString::fromUtf8("无法开始：")+db.error().left(160);
                    if(ok){deadline=db.testDeadline();next=s.mono+qint64(interval)*1000;failure=s.error;}
                }
            }else if(operation=="stop"){
                if(db.runningTest().isEmpty()||settings.value("test/target").toString()!=db.runningTest())note=QString::fromUtf8("测试已经结束");
                else{
                    Battery::Sample s=capture(run,interval);
                    QVariantMap t=db.query("SELECT mode FROM capacity_tests WHERE id=?",QVariantList()<<db.runningTest()).value(0).toMap();
                    QVariantMap prior=db.query("SELECT battery_id,utc_ms,mono_ms FROM samples ORDER BY id DESC LIMIT 1").value(0).toMap();
                    QString reason;
                    if(s.batteryId!=prior["battery_id"].toInt())reason="battery-changed";
                    else if(qAbs((s.utc-prior["utc_ms"].toLongLong())-(s.mono-prior["mono_ms"].toLongLong()))>2000)reason="clock-change";
                    else if(!s.ready||s.mode!=t["mode"].toString())reason="state-changed";
                    ok=reason.isEmpty()?db.finishTest(s.mono>=deadline?"timer":"manual",true,request,&s):db.finishTest(reason,false,request);
                    note=ok?QString::fromUtf8("测试已结束并保存"):QString::fromUtf8("结束失败，请重试");
                }
            }else note=QString::fromUtf8("未知测试操作");
            // Successful start/stop and its acknowledgement commit together.
            if(db.query("SELECT request_id FROM test_control WHERE id=1").value(0).toMap()["request_id"]!=request
               &&!db.execute("INSERT OR REPLACE INTO test_control VALUES(1,?,?,?)",QVariantList()<<request<<int(ok)<<note))failure=db.error();
            lastHeartbeat=0;
        }
        now=Battery::monoMillis();
        if(batteryReady&&!db.runningTest().isEmpty()&&(now>=next||now>=deadline)){
            Battery::Sample s=capture(run,interval);
            QVariantMap t=db.query("SELECT mode FROM capacity_tests WHERE id=?",QVariantList()<<db.runningTest()).value(0).toMap();
            QVariantMap prior=db.query("SELECT battery_id,utc_ms,mono_ms FROM samples ORDER BY id DESC LIMIT 1").value(0).toMap();
            QString reason;
            if(s.batteryId!=prior["battery_id"].toInt())reason="battery-changed";
            else if(qAbs((s.utc-prior["utc_ms"].toLongLong())-(s.mono-prior["mono_ms"].toLongLong()))>2000)reason="clock-change";
            else if(!s.ready||s.mode!=t["mode"].toString())reason="state-changed";
            if(!reason.isEmpty()){
                if(!db.finishTest(reason,false))failure=db.error();
            }else if(db.append(s)){
                failure=s.error;
                if(s.mono>=deadline&&!db.finishTest("timer",true))failure=db.error();
            }else failure=db.error();
            next=s.mono+qint64(interval)*1000;
            if(db.runningTest().isEmpty())lastHeartbeat=0;
        }
        const bool idle=db.runningTest().isEmpty();
        if(now-lastHeartbeat>=(idle?30000:10000)||lastHeartbeat==0){
            if(!db.heartbeat(run,interval,idle,failure))std::fputs("Database heartbeat failed\n",stderr);
            lastHeartbeat=now;
        }
        // No event subscription or idle samples. Only poll for test commands.
        usleep(idle?2000000:1000000);
    }
    db.finishTest("collector-stopped",false);
    db.closeSession("collector-stopped");db.heartbeat(run,10,true,"Collector stopped");
    bps_shutdown();::close(lock);return 0;
}
