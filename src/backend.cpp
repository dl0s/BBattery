#include "backend.h"
#include "collector_state.h"
#include "capacity_summary.h"
#include <bb/cascades/Application>
#include <bb/cascades/Window>
#include <bb/system/Screenshot>
#include <bb/system/InvokeManager>
#include <bb/system/InvokeRequest>
#include <bb/system/InvokeTargetReply>
#include <bb/system/InvokeReplyError>
#include <bb/data/JsonDataAccess>
#include <QFile>
#include <QDir>
#include <QDateTime>
#include <QLocale>
#include <QTextStream>
#include <QSettings>
#include <QCryptographicHash>
#include <QThread>
#include <QProcess>
#include <QFileInfo>
#include <QCoreApplication>
#include <QMetaObject>
#include <QUuid>
#include <unistd.h>
#include <cstdio>

namespace {
QVariantMap first(const QVariantList &rows){return rows.isEmpty()?QVariantMap():rows.first().toMap();}
QString metric(const QVariant &v,int precision,const QString &unit=QString()){
    return v.isValid()&&!v.isNull()?QString::number(v.toDouble(),'f',precision)+unit:QString("--");
}
QString timeText(qint64 ms){return ms>0?QDateTime::fromMSecsSinceEpoch(ms).toString("yyyy-MM-dd HH:mm"):QString("--");}
QString duration(double seconds){
    int n=qMax(0,int(seconds));return QString("%1:%2:%3").arg(n/3600,2,10,QChar('0')).arg(n/60%60,2,10,QChar('0')).arg(n%60,2,10,QChar('0'));
}
QString csv(const QVariant &v){
    if(!v.isValid()||v.isNull())return QString();
    QString s=v.toString();s.replace("\"","\"\"");return "\""+s+"\"";
}
QString reasonText(const QString &reason){
    if(reason=="timer")return QString::fromUtf8("达到设定时长");
    if(reason=="manual")return QString::fromUtf8("手动结束");
    if(reason=="state-changed")return QString::fromUtf8("充放电状态改变");
    if(reason=="battery-changed"||reason=="battery-marker-changed")return QString::fromUtf8("电池已更换");
    if(reason=="clock-change")return QString::fromUtf8("系统时间发生变化");
    if(reason=="collector-restart"||reason=="collector-stopped")return QString::fromUtf8("采集器停止或设备重启");
    return reason;
}
QVariantMap formatCapacity(QVariantMap summary){
    const QString type=summary["sourceType"].toString();
    summary["valueText"]=summary["available"].toBool()?QLocale(QLocale::English).toString(qRound(summary["value"].toDouble())):QString();
    summary["sourceText"]=type=="discharge-test"?QString::fromUtf8("放电估计"):
        type=="charge-test"?QString::fromUtf8("充电估计"):type=="system-reference"?QString::fromUtf8("系统参考"):QString::fromUtf8("暂无可靠估计");
    summary["timeText"]=timeText(summary["estimatedAt"].toLongLong());
    summary["note"]=type=="system-reference"?QString::fromUtf8("系统满充容量参考值，来自已确认归属的系统记录；不是标称容量，也不是合格测试结果。"):
        summary["available"].toBool()?QString::fromUtf8("整电池容量参考值，按测试 SOC 跨度外推；与本次区间电量不同。充电和放电结果分别保留，不平均混合。"):
        QString::fromUtf8("尚无合格定时测试，且系统读数的实体电池归属未确认。装入此电池、设为当前后开始测试。需 SOC 变化至少 30 个百分点、稳定方向和至少 95% 电流积分覆盖。");
    return summary;
}
void updateModel(bb::cascades::ArrayDataModel *model,const QVariantList &items){
    // Replace individual rows to retain list position and keyboard focus on refresh.
    for(int i=0;i<items.size();++i){
        if(i>=model->size())model->append(items[i]);
        else if(model->value(i)!=items[i])model->replace(i,items[i]);
    }
    while(model->size()>items.size())model->removeAt(model->size()-1);
}
}
// Bounded indexed reads only. No history scans, curves or lifetime statistics.
class TestReader:public QThread {
public:
    explicit TestReader(QObject *parent):QThread(parent),limit(50),elapsed(0){}
    QString path,key,activeKey,selected,error;int limit;qint64 elapsed;
    QVariantMap state,sample,active,detail,control,reading;QVariantList rows,items,summaries;
protected:
    void run(){
        qint64 start=Battery::monoMillis();Battery::Store db;error.clear();rows.clear();
        state.clear();sample.clear();active.clear();detail.clear();control.clear();reading.clear();summaries.clear();
        if(!db.open(path,false)){error=db.error();return;}
        if(db.query("SELECT name FROM sqlite_master WHERE type='table' AND name='capacity_tests'").isEmpty()){
            error="Waiting for interval-test collector";return;
        }
        state=first(db.query("SELECT * FROM collector_state WHERE id=1"));
        active=first(db.query("SELECT t.*,b.label AS battery_label FROM capacity_tests t JOIN battery_profiles b ON b.key=t.battery_key WHERE t.status='running' LIMIT 1"));
        if(active.isEmpty())active=first(db.query("SELECT t.*,b.label AS battery_label FROM capacity_tests t JOIN battery_profiles b ON b.key=t.battery_key WHERE t.battery_key=? ORDER BY t.start_ms DESC LIMIT 1",QVariantList()<<activeKey));
        if(active.value("status")=="running")sample=first(db.query("SELECT s.* FROM test_samples x JOIN samples s ON s.id=x.sample_id WHERE x.test_id=? ORDER BY x.sample_id DESC LIMIT 1",QVariantList()<<active["id"]));
        rows=db.query("SELECT t.*,b.label AS battery_label FROM capacity_tests t JOIN battery_profiles b ON b.key=t.battery_key WHERE t.battery_key=? ORDER BY t.start_ms DESC LIMIT ?",QVariantList()<<key<<limit+1);
        if(!selected.isEmpty())detail=first(db.query("SELECT t.*,b.label AS battery_label FROM capacity_tests t JOIN battery_profiles b ON b.key=t.battery_key WHERE t.id=? AND t.battery_key=?",QVariantList()<<selected<<key));
        if(!detail.isEmpty()){
            QString validity=QString::fromUtf8(Battery::Capacity::summarySql().c_str());
            validity.replace("ORDER BY end_ms DESC,id DESC LIMIT 1","AND id=? LIMIT 1");
            if(db.query(validity,QVariantList()<<key<<detail["mode"]<<selected).isEmpty())detail["capacity"]=QVariant();
        }
        QVariantMap display=detail.isEmpty()?first(rows):detail;
        if(!display.isEmpty())reading=first(db.query("SELECT s.* FROM test_samples x JOIN samples s ON s.id=x.sample_id WHERE x.test_id=? ORDER BY x.sample_id DESC LIMIT 1",QVariantList()<<display["id"]));
        reading["live"]=display["status"]=="running"&&key==activeKey;
        QVariantList catalog=items;
        foreach(const QVariant &v,db.query("SELECT key,label FROM battery_profiles ORDER BY key")){
            const QVariantMap stored=v.toMap();bool found=false;
            for(int i=0;i<catalog.size();++i){QVariantMap entry=catalog[i].toMap();if(entry["key"]!=stored["key"])continue;
                found=true;if(!entry["configured"].toBool()){entry["label"]=stored["label"];catalog[i]=entry;}break;}
            if(!found){QVariantMap entry=stored;entry["active"]=stored["key"]==activeKey;entry["configured"]=false;catalog<<entry;}
        }
        foreach(const QVariant &v,catalog){
            QVariantMap item=v.toMap();item["capacity"]=formatCapacity(db.capacitySummary(item["key"].toString(),activeKey));
            item["testing"]=active["status"]=="running"&&active["battery_key"]==item["key"];
            summaries<<item;
        }
        control=first(db.query("SELECT * FROM test_control WHERE id=1"));error=db.error();elapsed=Battery::monoMillis()-start;
    }
};
class ExportWorker:public QThread {
public:
    explicit ExportWorker(QObject *parent):QThread(parent),all(false){}
    QString path,output,id,error;QVariantList batteries;bool all;
protected:
    void run(){
        error.clear();Battery::Store db;
        if(!db.open(path,false)||!QDir().mkpath(output)){error="Export storage unavailable";return;}
        const QString base=output+"/battery-"+QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss")+"-"+QUuid::createUuid().toString().mid(1,8);
        QFile file(base+"-samples.csv");if(!file.open(QIODevice::WriteOnly)){error="CSV open failed";return;}
        QTextStream out(&file);out.setCodec("UTF-8");out.setGenerateByteOrderMark(true);
        const QStringList names=QString("id,utc_ms,mono_ms,run_id,session_id,interval_s,ready,battery_id,mode,charger,soc,current,voltage,temperature,gap_reason,error,battery_key,battery_label").split(',');
        out<<names.join(",")<<"\n";
        const qint64 maximum=first(db.query("SELECT MAX(id) AS n FROM samples"))["n"].toLongLong();qint64 cursor=0;
        while(cursor<maximum){
            QString sql="SELECT s.*,b.battery_key,p.label AS battery_label FROM samples s JOIN session_batteries b ON b.session_id=s.session_id JOIN battery_profiles p ON p.key=b.battery_key WHERE s.id>? AND s.id<=?";
            QVariantList values;values<<cursor<<maximum;
            if(!all){sql+=" AND s.id IN (SELECT sample_id FROM test_samples WHERE test_id=?)";values<<id;}
            sql+=" ORDER BY s.id LIMIT 512";QVariantList rows=db.query(sql,values);
            if(!db.error().isEmpty()){error=db.error();break;}
            if(rows.isEmpty())break;
            foreach(const QVariant &v,rows){QVariantMap row=v.toMap();QStringList fields;foreach(const QString &name,names)fields<<csv(row[name]);out<<fields.join(",")<<"\n";cursor=row["id"].toLongLong();}
            usleep(1000);
        }
        out.flush();if(out.status()!=QTextStream::Ok)error="CSV write failed";file.close();
        if(!error.isEmpty()){file.remove();return;}
        bb::data::JsonDataAccess json;QVariantMap report;
        report["test"]=all?QVariantMap():first(db.query("SELECT * FROM capacity_tests WHERE id=?",QVariantList()<<id));
        if(all)report["tests"]=db.query("SELECT * FROM capacity_tests ORDER BY start_ms");
        report["batteries"]=batteries;report["samplesFile"]=file.fileName();
        report["units"]="current=mA, voltage=mV, capacity=mAh, energy=mWh";
        report["method"]="Adjacent battery average current integrated within monotonic test boundaries; gaps excluded. Full capacity is an SOC-based estimate only when completed, SOC span >=30 points and current coverage >=95%. CSV endpoint may be after the deadline; result integration is clipped at deadline.";
        report["missingValues"]="Empty fields are unavailable, not zero";
        json.save(report,base+"-result.json");if(json.hasError())error="Result write failed";
    }
};
Backend::Backend():records(new bb::cascades::ArrayDataModel(this)),profiles(new bb::cascades::ArrayDataModel(this)),reader(new TestReader(this)),exporter(new ExportWorker(this)),
    scene(0),limit(50),busy(false),again(false),collectorReady(false),more(false),collectorLaunchAttempted(false),
    collectorInvoker(new bb::system::InvokeManager(this)),collectorInvocation(0),pendingExpires(0){
    directory=QDir::currentPath()+"/data/battery";QDir().mkpath(directory);QDir().mkpath("data/diagnostics");
    bb::data::JsonDataAccess json;QVariantMap identity;identity["pid"]=qint64(getpid());identity["uid"]=qint64(getuid());identity["euid"]=qint64(geteuid());
    json.save(identity,QDir::currentPath()+"/data/gui-identity.json");
    message=QString::fromUtf8("正在连接测试采集器");loadBatteries();
    QSettings intent(directory+"/settings.ini",QSettings::IniFormat);
    pendingRequest=intent.value("test/request").toString();pendingExpires=intent.value("test/expires").toLongLong();
    connect(&refreshTimer,SIGNAL(timeout()),this,SLOT(refresh()));refreshTimer.start(10000);
    connect(&diagnosticTimer,SIGNAL(timeout()),this,SLOT(diagnostic()));diagnosticTimer.start(1000);
    connect(reader,SIGNAL(finished()),this,SLOT(readReady()));connect(exporter,SIGNAL(finished()),this,SLOT(exportReady()));
    QTimer::singleShot(0,this,SLOT(refresh()));
}
Backend::~Backend(){reader->wait();exporter->wait();}
void Backend::refresh(){
    loadBatteries();if(busy){again=true;return;}
    reader->path=directory+"/history.sqlite";reader->key=viewedBattery;reader->activeKey=activeBattery;reader->selected=selectedId;reader->limit=limit;
    reader->items=batteryItems;
    busy=true;again=false;reader->start();
}
bool Backend::connectCollector(){
    if(collectorInvocation){message=QString::fromUtf8("正在等待系统启动采集器，请稍候");emit changed();return false;}
    const Battery::CollectorHealth health=Battery::collectorHealth(directory,collectorState);
    if(health!=Battery::CollectorStopped){
        message=QString::fromUtf8("采集器实例尚未就绪，请刷新核对；不会重复启动");emit changed();return false;
    }
    bb::system::InvokeRequest invocation;
    invocation.setTarget("top.blaccat.BBattery.collector");
    invocation.setAction("top.blaccat.BBattery.START_COLLECTOR");
    invocation.setMimeType("application/vnd.blaccat.bbattery.collector");
    collectorInvocation=collectorInvoker->invoke(invocation);
    if(!collectorInvocation){
        message=QString::fromUtf8("系统无法启动采集器，测试不可用；请核对安装包的后台权限并重新打开");emit changed();return false;
    }
    connect(collectorInvocation,SIGNAL(finished()),this,SLOT(collectorInvocationFinished()));
    // OS headless entry point, same app identity; readiness still needs lock/instance/heartbeat.
    message=QString::fromUtf8("采集器已请求启动，正在核对实例和业务心跳");refreshTimer.setInterval(1000);QTimer::singleShot(1500,this,SLOT(refresh()));emit changed();return true;
}
void Backend::collectorInvocationFinished(){
    if(!collectorInvocation)return;
    bb::system::InvokeTargetReply *reply=collectorInvocation;collectorInvocation=0;
    if(reply->error()!=bb::system::InvokeReplyError::None){
        message=QString::fromUtf8("系统拒绝采集器启动，测试不可用；请核对本包后台权限（错误 %1）").arg(int(reply->error()));
        emit notified(message);emit changed();
    }
    reply->deleteLater();refresh();
}
QVariantMap Backend::formatTest(const QVariantMap &row)const{
    if(row.isEmpty())return row;
    QVariantMap r=row;bool active=row["status"]=="running";
    double elapsed=row["elapsed_s"].toDouble();
    const double coverage=elapsed>0?100*row["integrated_s"].toDouble()/elapsed:0;
    r["startText"]=timeText(row["start_ms"].toLongLong());r["endText"]=timeText(row["end_ms"].toLongLong());
    r["elapsedText"]=duration(elapsed);r["plannedText"]=duration(row["requested_s"].toDouble());
    r["remainingText"]=duration(qMax(0.0,(row["deadline_mono"].toLongLong()-Battery::monoMillis())/1000.0));
    r["modeText"]=row["mode"]=="charge"?QString::fromUtf8("充电测试"):QString::fromUtf8("放电测试");
    r["stateText"]=active?QString::fromUtf8("正在测试"):row["status"]=="completed"?QString::fromUtf8("已完成"):QString::fromUtf8("已中断");
    r["reasonText"]=reasonText(row["reason"].toString());
    bool charging=row["mode"]=="charge";
    r["mahText"]=row["integrated_s"].toDouble()>0?metric(row[charging?"charge_mah":"discharge_mah"],1," mAh"):QString("--");
    r["mwhText"]=row["energy_s"].toDouble()>0?metric(row[charging?"charge_mwh":"discharge_mwh"],1," mWh"):QString("--");
    r["socText"]=metric(row["start_soc"],0,"%")+" → "+metric(row["end_soc"],0,"%");
    r["coverageText"]=QString::number(qBound(0.0,coverage,100.0),'f',1)+"%";
    r["meanCurrentText"]=row["integrated_s"].toDouble()>0?metric(QVariant((charging?1:-1)*row[charging?"charge_mah":"discharge_mah"].toDouble()*3600/row["integrated_s"].toDouble()),0," mA"):QString("--");
    r["gapsText"]=row["gaps"].toString();
    r["capacityText"]=metric(row["capacity"],0," mAh");
    r["estimateReady"]=!row["capacity"].isNull()&&row["capacity"].isValid();
    r["estimateNote"]=r["estimateReady"].toBool()?QString::fromUtf8("依据本次电量变化外推，仅作参考"):
        active?QString::fromUtf8("结束后评估整电池容量"):
        row["status"]!="completed"?QString::fromUtf8("测试已中断，保留有效区间电量"):
        row["start_soc"].isNull()||row["end_soc"].isNull()?QString::fromUtf8("系统未提供完整电量读数"):
        !row["stable_soc"].toBool()?QString::fromUtf8("电量变化不稳定，无法外推"):
        coverage<95?QString::fromUtf8("有效采样覆盖不足 95%，无法外推"):
        QString::fromUtf8("需电量变化至少 30 个百分点才可外推");
    r["title"]=r["startText"].toString()+" · "+r["stateText"].toString();
    r["subtitle"]=r["modeText"].toString()+" · "+r["elapsedText"].toString()+" · "+r["mahText"].toString();
    return r;
}
void Backend::readReady(){
    reader->wait();busy=false;
    if(reader->key!=viewedBattery||reader->activeKey!=activeBattery||reader->selected!=selectedId){refresh();return;}
    const bool previouslyReady=collectorReady;
    collectorReady=reader->error.isEmpty()&&Battery::collectorHealth(directory,reader->state)==Battery::CollectorReady;
    if(reader->error.isEmpty()&&!pendingRequest.isEmpty()&&reader->control["request_id"]==pendingRequest){
        message=reader->control["message"].toString();pendingRequest.clear();emit notified(message);
    }else if(!pendingRequest.isEmpty()&&Battery::utcMillis()>pendingExpires){
        message=QString::fromUtf8("操作待确认，恢复连接后继续核对");
    }else if(!collectorReady){
        message=reader->error.isEmpty()?QString::fromUtf8("采集器未就绪，测试暂不可用；进入测试页重新连接"):
            QString::fromUtf8("历史读取失败，请刷新重试；数据仍保留：")+reader->error.left(200);
    }else if(!previouslyReady)message=QString::fromUtf8("就绪 · 仅在测试期间采样");
    collectorState=reader->state;currentTest=formatTest(reader->active);selected=formatTest(reader->detail);current=reader->sample;
    if(!collectorReady&&!collectorLaunchAttempted){collectorLaunchAttempted=true;connectCollector();}
    current["socText"]=metric(current["soc"],0,"%");current["currentText"]=metric(current["current"],0," mA");
    current["temperatureText"]=metric(current["temperature"],1," °C");current["voltageText"]=metric(current["voltage"],0," mV");
    current["lastText"]=timeText(current["utc_ms"].toLongLong());current["fresh"]=collectorReady&&running()&&Battery::utcMillis()-current["utc_ms"].toLongLong()>=0&&Battery::utcMillis()-current["utc_ms"].toLongLong()<qMax(90,collectorState["interval_s"].toInt()*3)*1000;
    viewedReading=reader->reading;
    viewedReading["live"]=viewedReading["live"].toBool()&&current["fresh"].toBool();
    viewedReading["socText"]=metric(viewedReading["soc"],0,"%");
    viewedReading["currentText"]=metric(viewedReading["current"],0," mA");
    viewedReading["temperatureText"]=metric(viewedReading["temperature"],1," °C");
    viewedReading["voltageText"]=metric(viewedReading["voltage"],0," mV");
    viewedReading["lastText"]=timeText(viewedReading["utc_ms"].toLongLong());
    if(reader->error.isEmpty()){
        if(reader->items!=batteryItems){refresh();return;}
        QSettings restored(directory+"/settings.ini",QSettings::IniFormat);bool imported=false;
        for(int i=0;i<reader->summaries.size();++i){QVariantMap item=reader->summaries[i].toMap();
            if(!item["configured"].toBool()){
                restored.setValue("batteries/"+item["key"].toString(),item["label"]);imported=true;item["configured"]=true;reader->summaries[i]=item;}}
        if(imported){restored.sync();if(restored.status()!=QSettings::NoError)message=QString::fromUtf8("历史档案已读取，但设置保存失败；请检查存储后重试");}
        if(reader->summaries!=batteryItems){batteryItems=reader->summaries;updateModel(profiles,batteryItems);}
        if(imported)loadBatteries();
        foreach(const QVariant &v,batteryItems)if(v.toMap()["key"]==viewedBattery)viewedCapacity=v.toMap()["capacity"].toMap();
        emit batteriesChanged();
    }else{
        for(int i=0;i<batteryItems.size();++i){QVariantMap item=batteryItems[i].toMap();QVariantMap summary=item["capacity"].toMap();
            if(!summary["available"].toBool()){summary["unavailableReason"]="database-read-failed";item["capacity"]=summary;batteryItems[i]=item;}}
        updateModel(profiles,batteryItems);emit batteriesChanged();
        foreach(const QVariant &v,batteryItems)if(v.toMap()["key"]==viewedBattery)viewedCapacity=v.toMap()["capacity"].toMap();
    }
    more=reader->rows.size()>limit;QVariantList raw=reader->rows;if(more)raw.removeLast();
    if(raw!=recordRows){recordRows=raw;QVariantList formatted;foreach(const QVariant &v,raw)formatted<<formatTest(v.toMap());records->clear();records->append(formatted);}
    if(!db.handle())db.open(directory+"/history.sqlite",false);
    refreshTimer.setInterval(pending()?1000:running()?5000:10000);
    emit changed();if(again)refresh();
}
bool Backend::request(const QString &operation,int seconds,int interval){
    if(pending()||!ready())return false;
    const QString id=QUuid::createUuid().toString().mid(1,36);
    QSettings settings(directory+"/settings.ini",QSettings::IniFormat);
    settings.setValue("test/operation",operation);settings.setValue("test/seconds",seconds);settings.setValue("test/interval",interval);
    settings.setValue("test/battery",activeBattery);settings.setValue("test/target",currentTest["id"]);
    settings.setValue("test/digest",QString::fromLatin1(QCryptographicHash::hash(
        (operation+"\n"+QString::number(seconds)+"\n"+QString::number(interval)+"\n"+activeBattery+"\n"+currentTest["id"].toString()).toUtf8(),QCryptographicHash::Sha1).toHex()));
    pendingExpires=Battery::utcMillis()+20000;settings.setValue("test/expires",pendingExpires);settings.setValue("test/request",id);settings.sync();
    if(settings.status()!=QSettings::NoError){message=QString::fromUtf8("测试操作保存失败");emit changed();return false;}
    pendingRequest=id;message=QString::fromUtf8("正在等待采集器确认");refreshTimer.setInterval(1000);emit changed();refresh();return true;
}
bool Backend::startTest(int minutes,int interval){
    if(minutes<1||minutes>1440||(interval!=10&&interval!=30&&interval!=60)||running())return false;
    return request("start",minutes*60,interval);
}
bool Backend::stopTest(){return running()&&request("stop",0,collectorState.value("interval_s",30).toInt());}
void Backend::loadBatteries(){
    QSettings settings(directory+"/settings.ini",QSettings::IniFormat);settings.beginGroup("batteries");QStringList keys=settings.childKeys();settings.endGroup();
    if(!keys.contains("legacy"))keys.prepend("legacy");QString active=settings.value("battery/active","legacy").toString();if(!keys.contains(active))active="legacy";
    activeBattery=active;if(viewedBattery.isEmpty()||!keys.contains(viewedBattery))viewedBattery=active;
    QVariantList items;QVariantMap state;
    foreach(const QString &key,keys){QVariantMap item;
        foreach(const QVariant &old,batteryItems)if(old.toMap()["key"]==key){item=old.toMap();break;}
        if(!item.contains("capacity")){QVariantMap empty;empty["available"]=false;empty["sourceType"]="none";empty["unavailableReason"]="loading";empty["value"]=QVariant();item["capacity"]=formatCapacity(empty);}
        item["key"]=key;item["active"]=key==active;item["configured"]=settings.contains("batteries/"+key);item["label"]=settings.value("batteries/"+key,QString::fromUtf8("电池 1")).toString();items<<item;
        if(key==active)state["activeLabel"]=item["label"];if(key==viewedBattery)state["viewedLabel"]=item["label"];}
    state["activeKey"]=active;state["viewedKey"]=viewedBattery;
    foreach(const QVariant &v,items)if(v.toMap()["key"]==viewedBattery)viewedCapacity=v.toMap()["capacity"].toMap();
    if(items!=batteryItems||state!=batteryState){batteryItems=items;batteryState=state;updateModel(profiles,batteryItems);emit batteriesChanged();}
}
QString Backend::createBattery(const QString &name){
    const QString label=name.trimmed();if(label.isEmpty()||label.size()>40||label.contains('\n')||label.contains('\r')||label.contains('\t')){emit notified(QString::fromUtf8("请输入 1–40 字的电池标记"));return QString();}
    foreach(const QVariant &v,batteryItems)if(v.toMap()["label"].toString().compare(label,Qt::CaseInsensitive)==0){emit notified(QString::fromUtf8("此标记已存在，请选择原有电池"));return QString();}
    QString key=QUuid::createUuid().toString().mid(1,36);QSettings settings(directory+"/settings.ini",QSettings::IniFormat);settings.setValue("batteries/"+key,label);settings.sync();
    if(settings.status()!=QSettings::NoError)return QString();loadBatteries();viewBattery(key);return key;
}
bool Backend::renameBattery(const QString &key,const QString &name){
    QString label=name.trimmed();bool found=false;if(label.isEmpty()||label.size()>40||label.contains('\n')||label.contains('\r')||label.contains('\t'))return false;
    foreach(const QVariant &v,batteryItems){QVariantMap row=v.toMap();if(row["key"]==key)found=true;else if(row["label"].toString().compare(label,Qt::CaseInsensitive)==0)return false;}
    if(!found)return false;QSettings settings(directory+"/settings.ini",QSettings::IniFormat);settings.setValue("batteries/"+key,label);settings.sync();if(settings.status()!=QSettings::NoError)return false;
    loadBatteries();refresh();return true;
}
bool Backend::useBattery(const QString &key){
    if(running()||pending()){emit notified(QString::fromUtf8("请先结束测试并确认原操作，再切换电池"));return false;}
    if(!ready()){emit notified(QString::fromUtf8("采集器未就绪，无法核对测试状态；请恢复采集器后再切换"));return false;}
    bool found=false;foreach(const QVariant &v,batteryItems)if(v.toMap()["key"]==key)found=true;if(!found)return false;
    QSettings settings(directory+"/settings.ini",QSettings::IniFormat);settings.setValue("battery/active",key);settings.sync();if(settings.status()!=QSettings::NoError)return false;
    loadBatteries();viewBattery(key);refresh();emit notified(QString::fromUtf8("已设为当前电池，装回同一电池时选择此标记"));return true;
}
void Backend::viewBattery(const QString &key){
    bool found=false;foreach(const QVariant &v,batteryItems)if(v.toMap()["key"]==key)found=true;if(!found||viewedBattery==key)return;
    viewedBattery=key;selectedId.clear();selected.clear();viewedReading.clear();recordRows.clear();records->clear();
    limit=50;loadBatteries();emit changed();refresh();
}
void Backend::selectTest(const QString &id){selectedId=id;selected.clear();viewedReading.clear();emit changed();refresh();}
void Backend::loadMore(){limit+=50;refresh();}
void Backend::exportData(bool allHistory){
    if(exporter->isRunning()){emit notified(QString::fromUtf8("正在导出，请稍候"));return;}
    if(!allHistory&&selectedId.isEmpty()){emit notified(QString::fromUtf8("请先选择一项测试结果"));return;}
    exporter->path=directory+"/history.sqlite";exporter->output=QDir::currentPath()+"/shared/documents/BBattery";
    exporter->id=selectedId;exporter->batteries=batteryItems;exporter->all=allHistory;exporter->start();emit notified(QString::fromUtf8("正在导出"));
}
void Backend::exportReady(){exporter->wait();emit notified(exporter->error.isEmpty()?QString::fromUtf8("已导出至 Documents/BBattery"):QString::fromUtf8("导出失败：")+exporter->error);}
void Backend::captureScreen(){
    QString folder=QDir::currentPath()+"/shared/documents/BBattery";
    if(!QDir().mkpath(folder)){emit notified(QString::fromUtf8("截图目录不可用，请检查共享存储"));return;}
    const QString stem=folder+"/ui-"+QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss")+"-"+QUuid::createUuid().toString().mid(1,8);
    bb::system::Screenshot capture;
    const QString path=capture.captureWindow(QUrl::fromLocalFile(stem+".png"),bb::cascades::Application::instance()->mainWindow()->handle());
    if(path.isEmpty()){emit notified(QString::fromUtf8("截图未保存，请重试；测试继续执行"));return;}
    bb::data::JsonDataAccess json;QVariantMap evidence;
    evidence["applicationVersion"]="0.1.0.12";evidence["ready"]=ready();evidence["pending"]=pending();evidence["running"]=running();
    evidence["battery"]=batteryState;evidence["batteries"]=batteryItems;evidence["test"]=currentTest;evidence["detail"]=selected;
    evidence["reading"]=viewedReading;evidence["collector"]=collectorState;evidence["euid"]=qint64(geteuid());
    evidence["capturedAt"]=Battery::utcMillis();evidence["source"]="native-application-window";json.save(evidence,stem+".json");
    emit notified(QString::fromUtf8("截图已保存至 Documents/BBattery"));
}
void Backend::diagnostic(){
    QFile file("data/diagnostics/request.json");if(!file.open(QIODevice::ReadOnly))return;
    bb::data::JsonDataAccess json;QVariantMap args=json.loadFromBuffer(file.readAll()).toMap();file.close();file.remove();
    QVariantMap response;response["requestId"]=args["requestId"];response["ok"]=true;QString op=args["op"].toString();
    if(op=="state"){
        response["test"]=currentTest;response["detail"]=selected;response["live"]=current;response["battery"]=batteryState;response["batteries"]=batteryItems;
        response["ready"]=ready();response["pending"]=pending();response["running"]=running();response["status"]=message;response["rows"]=resultCount();response["refreshing"]=busy;
        response["readMs"]=reader->elapsed;response["collector"]=collectorState;response["chartRenders"]=0;response["euid"]=qint64(geteuid());
    }else if(op=="start-test")response["ok"]=startTest(args["minutes"].toInt(),args.value("interval",30).toInt());
    else if(op=="stop-test")response["ok"]=stopTest();
    else if(op=="select-test")selectTest(args["id"].toString());
    else if(op=="create-battery"){response["key"]=createBattery(args["label"].toString());response["ok"]=!response["key"].toString().isEmpty();}
    else if(op=="rename-battery")response["ok"]=renameBattery(args["key"].toString(),args["label"].toString());
    else if(op=="use-battery")response["ok"]=useBattery(args["key"].toString());
    else if(op=="view-battery")viewBattery(args["key"].toString());
    else if(op=="refresh")refresh();
    else if(op=="export")exportData(args.value("all",false).toBool());
    else if(op=="view"){QVariant layout;response["ok"]=scene&&QMetaObject::invokeMethod(scene,"testView",Q_RETURN_ARG(QVariant,layout),Q_ARG(QVariant,QVariant(args)));response["layout"]=layout;}
    else if(op=="capture"){
        bb::system::Screenshot capture;QString path=capture.captureWindow(QUrl::fromLocalFile(QDir::currentPath()+"/data/diagnostics/ui.png"),bb::cascades::Application::instance()->mainWindow()->handle());response["ok"]=!path.isEmpty();response["path"]=path;
    }else if(op=="backup"){
        QString path=QDir::currentPath()+"/data/diagnostics/history.sqlite";sqlite3 *destination=0;bool ok=db.handle()&&sqlite3_open(QFile::encodeName(path).constData(),&destination)==SQLITE_OK;
        if(ok){sqlite3_backup *copy=sqlite3_backup_init(destination,"main",db.handle(),"main");int rc=copy?sqlite3_backup_step(copy,-1):SQLITE_ERROR;int finish=copy?sqlite3_backup_finish(copy):SQLITE_ERROR;ok=rc==SQLITE_DONE&&finish==SQLITE_OK;}
        if(destination)sqlite3_close(destination);response["ok"]=ok;response["path"]=path;
    }else{response["ok"]=false;response["error"]="Unknown finite diagnostic operation";}
    writeResponse(response);
}
void Backend::writeResponse(const QVariantMap &response){bb::data::JsonDataAccess json;json.save(response,QDir::currentPath()+"/data/diagnostics/response.json");}
