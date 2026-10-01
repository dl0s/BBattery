#include "backend.h"
#include <bb/ImageData>
#include <bb/PixelFormat>
#include <bb/cascades/Application>
#include <bb/cascades/Window>
#include <bb/system/Screenshot>
#include <bb/data/JsonDataAccess>
#include <QImage>
#include <QPainter>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDateTime>
#include <QTextStream>
#include <QMetaObject>
#include <QSettings>
#include <QThread>
#include <QUuid>
#include <qnumeric.h>
#include <unistd.h>
#include <cmath>
#include <algorithm>
#include <cstdio>

class HistoryLoader:public QThread {
public:
    explicit HistoryLoader(QObject *parent):QThread(parent),begin(0),end(0),generation(0){}
    QString path; qint64 begin,end;int generation;
    QVariantMap summary;
    QVariantList points,events;
protected:
    void run(){
        Battery::Store reader;summary.clear();points.clear();events.clear();
        if(!reader.open(path,false)){summary["complete"]=false;return;}
        points=reader.history(begin,end,&summary);
        events=reader.query("SELECT * FROM events WHERE utc_ms>=? AND utc_ms<=? ORDER BY utc_ms",QVariantList()<<begin<<end);
    }
};

class InspectionLoader:public QThread {
public:
    explicit InspectionLoader(QObject *parent):QThread(parent),begin(0),end(0),target(0),generation(0){}
    QString path,error;
    qint64 begin,end,target;
    int generation;
    QVariantList nearby;
protected:
    void run(){
        Battery::Store reader;nearby.clear();error.clear();
        if(!reader.open(path,false)){error=reader.error();return;}
        const QString columns="id,utc_ms,interval_s,soc,current,voltage,temperature,mode";
        nearby+=reader.query("SELECT "+columns+" FROM samples WHERE utc_ms<=? AND utc_ms>=? ORDER BY utc_ms DESC,id DESC LIMIT 1",
            QVariantList()<<target<<begin);
        nearby+=reader.query("SELECT "+columns+" FROM samples WHERE utc_ms>=? AND utc_ms<=? ORDER BY utc_ms,id LIMIT 1",
            QVariantList()<<target<<end);
        error=reader.error();
    }
};

namespace {
QString metric(const QVariant &value,int precision,const QString &unit=QString()){
    return value.isValid()&&!value.isNull()?QString::number(value.toDouble(),'f',precision)+unit:QString::fromUtf8("未提供");
}
QString timeText(qint64 value,const char *format="MM-dd HH:mm"){
    return QDateTime::fromMSecsSinceEpoch(value).toString(format);
}
QString duration(double seconds){
    if(seconds<60)return QString::fromUtf8("%1 秒").arg(int(seconds));
    if(seconds<3600)return QString::fromUtf8("%1 分").arg(int(seconds/60));
    return QString::fromUtf8("%1 小时 %2 分").arg(int(seconds/3600)).arg(int(seconds/60)%60);
}
QColor modeColor(const QString &mode){
    if(mode=="charge")return QColor("#63d6a0");
    if(mode=="discharge")return QColor("#78c9ee");
    if(mode=="plugged")return QColor("#efc56a");
    return QColor("#85898e");
}
QVariantMap first(const QVariantList &rows){return rows.isEmpty()?QVariantMap():rows.first().toMap();}
QString csvValue(const QVariant &value){
    if(!value.isValid()||value.isNull())return QString();
    QString t=value.toString();if(t.contains(',')||t.contains('"')||t.contains('\n')){t.replace("\"","\"\"");return "\""+t+"\"";}
    return t;
}
}
Backend::Backend():records(new bb::cascades::ArrayDataModel(this)),eventRecords(new bb::cascades::ArrayDataModel(this)),
    hours(1),sampleInterval(10),isPaused(false),sampleId(-1),detailId(0),
    endTime(0),rangeBegin(0),rangeEnd(0),inspectionTime(0),lastEventId(-1),scene(0),
    historyLoader(new HistoryLoader(this)),inspectionLoader(new InspectionLoader(this)),
    historyGeneration(0),loadedGeneration(-1),chartRenderCount(0),nextHistoryRefresh(0),
    historyPending(false),historyBusy(false),requestedInspectionTime(0),cachedInspectionTarget(0),
    cachedInspectionBegin(0),cachedInspectionEnd(0),inspectionBenchmarkStarted(0),
    inspectionGeneration(0),inspectionLookupCount(0),inspectionBenchmarkLookups(0),
    inspectionPending(false),inspectionBusy(false),inspectionCacheValid(false){
    std::fputs("BBattery backend: initialize\n",stderr);std::fflush(stderr);
    capacitySessionId=-1;capacityBatteryId=-1;
    directory=QDir::currentPath()+"/data/battery";channel="current";
    QDir().mkpath(directory);QDir().mkpath("data/diagnostics");
    QFile runtime("data/gui-identity.json");bb::data::JsonDataAccess json;
    QVariantMap identity;identity["pid"]=qint64(getpid());identity["uid"]=qint64(getuid());identity["euid"]=qint64(geteuid());
    QByteArray out;json.saveToBuffer(identity,&out);
    if(runtime.open(QIODevice::WriteOnly)){runtime.write(out);runtime.close();}
    if(!QFile::exists(directory+"/settings.ini"))saveSettings(10,false);
    connect(&refreshTimer,SIGNAL(timeout()),this,SLOT(refresh()));refreshTimer.start(2000);
    connect(&diagnosticTimer,SIGNAL(timeout()),this,SLOT(diagnostic()));diagnosticTimer.start(400);
    connect(historyLoader,SIGNAL(finished()),this,SLOT(historyReady()));
    inspectionTimer.setSingleShot(true);
    connect(&inspectionTimer,SIGNAL(timeout()),this,SLOT(processInspection()));
    connect(inspectionLoader,SIGNAL(finished()),this,SLOT(inspectionReady()));
    refresh();
    std::fputs("BBattery backend: ready\n",stderr);std::fflush(stderr);
}
Backend::~Backend(){historyLoader->wait();inspectionLoader->wait();}
QVariantMap Backend::inspectionCursor()const{
    QVariantMap cursor;
    bool visible=inspectionTime>0&&rangeEnd>rangeBegin&&inspectionTime>=rangeBegin&&inspectionTime<=rangeEnd;
    cursor["visible"]=visible;
    cursor["ratio"]=visible?double(inspectionTime-rangeBegin)/(rangeEnd-rangeBegin):0.0;
    return cursor;
}
void Backend::refresh(){
    QSettings settings(directory+"/settings.ini",QSettings::IniFormat);
    alertSettings["enabled"]=settings.value("alerts/enabled",true).toBool();
    alertSettings["low"]=settings.value("alerts/low",20).toInt();
    alertSettings["high"]=settings.value("alerts/high",90).toInt();
    alertSettings["temperature"]=settings.value("alerts/temperature",45).toInt();
    if(!db.handle()&&!db.open(directory+"/history.sqlite",false)){
        message=QString::fromUtf8("等待独立采集器");current["collectorText"]=message;current["fresh"]=false;
        current["socText"]="--";current["currentText"]="--";current["voltageText"]="--";current["temperatureText"]="--";
        current["lastText"]=QString::fromUtf8("尚无记录");current["modeText"]=QString::fromUtf8("尚未开始");
        assessment["summary"]=QString::fromUtf8("尚未收到采集数据");updateCharts();emit changed();return;
    }
    QVariantMap state=first(db.query("SELECT * FROM collector_state WHERE id=1"));
    sampleInterval=state.value("interval_s",10).toInt();isPaused=state.value("paused",0).toBool();
    const qint64 age=Battery::monoMillis()-state.value("mono_ms",0).toLongLong();
    bool fresh=!state.isEmpty()&&age>=0&&age<=qMax(15000,sampleInterval*3000);
    current["fresh"]=fresh;current["paused"]=isPaused;
    current["collectorText"]=isPaused?QString::fromUtf8("采集已暂停"):fresh?
        QString::fromUtf8("持续采集 · %1 秒").arg(sampleInterval):QString::fromUtf8("采集器失联");
    current["collectorAge"]=age>=0?duration(age/1000.0):QString::fromUtf8("时钟异常");
    QVariantMap s=first(db.query("SELECT samples.*,snapshots.raw FROM samples LEFT JOIN snapshots ON snapshots.id=samples.snapshot_id ORDER BY samples.id DESC LIMIT 1"));
    if(s.isEmpty()){message=QString::fromUtf8("等待首个采样");emit changed();return;}
    qint64 id=s["id"].toLongLong();
    current["socText"]=s["soc"].isNull()?"--":QString::number(s["soc"].toInt());
    current["socAvailable"]=s["soc"].isValid()&&!s["soc"].isNull();
    current["currentText"]=metric(s["current"],0);
    current["voltageText"]=s["voltage"].isNull()?QString::fromUtf8("未提供"):QString::number(s["voltage"].toDouble()/1000.0,'f',3);
    current["temperatureText"]=metric(s["temperature"],1);
    current["designText"]=metric(s["design"],0," mAh");
    current["fullText"]=metric(s["full_capacity"],0," mAh");
    current["healthText"]=metric(s["health"],0,"%");
    current["cyclesText"]=metric(s["cycles"],0);
    current["mode"]=s["mode"];current["modeText"]=Battery::modeLabel(s["mode"].toString());
    current["lastText"]=timeText(s["utc_ms"].toLongLong(),"HH:mm:ss");
    current["sampleAge"]=duration(qMax(qint64(0),Battery::utcMillis()-s["utc_ms"].toLongLong())/1000.0);
    current["externalPower"]=Battery::connected(s["charger"].toString());
    current["chargerText"]=s["charger"]=="NONE"?QString::fromUtf8("电池供电"):
        Battery::connected(s["charger"].toString())?QString::fromUtf8("已接外部电源"):QString::fromUtf8("电源状态未知");
    current["chargerRaw"]=s["charger"];current["phase"]=s["phase"];
    current["ready"]=s["ready"];
    QStringList warnings;
    if(alertSettings["enabled"].toBool()&&fresh&&!isPaused&&s["ready"].toBool()){
        if(!s["soc"].isNull()&&s["soc"].toDouble()<=alertSettings["low"].toInt())warnings<<QString::fromUtf8("低电量");
        if(!s["soc"].isNull()&&current["externalPower"].toBool()&&s["soc"].toDouble()>=alertSettings["high"].toInt())
            warnings<<QString::fromUtf8("已达充电提醒电量");
        if(!s["temperature"].isNull()&&s["temperature"].toDouble()>=alertSettings["temperature"].toInt())
            warnings<<QString::fromUtf8("电池温度偏高");
    }
    current["alertText"]=warnings.join(QString::fromUtf8(" · "));
    current["remainingText"]=metric(s["remaining"],0," mAh");
    current["inputText"]=metric(s["input_current"],0," mA");
    current["inputLimitText"]=metric(s["input_limit"],0," mA");
    current["chargeLimitText"]=metric(s["charge_limit"],0," mA");
    current["sourceError"]=s["error"];
    QVariantMap source=Battery::sourceDetails(s["raw"].toString());
    current["systemChargeText"]=metric(source["systemChargeCurrent"],0," mA");
    current["timeToEmptyText"]=source["timeToEmpty"].isValid()?duration(source["timeToEmpty"].toDouble()*60):QString::fromUtf8("未提供");
    current["timeToFullText"]=source["timeToFull"].isValid()?duration(source["timeToFull"].toDouble()*60):QString::fromUtf8("不适用");
    QVariantMap totals=first(db.query("SELECT COUNT(*) AS n,SUM(current IS NOT NULL) AS currents,SUM(voltage IS NOT NULL) AS voltages,"
        "SUM(temperature IS NOT NULL) AS temps,SUM(ready=0) AS unready,SUM(gap_reason<>'') AS gaps,MIN(utc_ms) AS first_ms,"
        "MAX(utc_ms) AS last_ms FROM samples"));
    int total=totals["n"].toInt();
    assessment["samples"]=total;assessment["gapCount"]=totals["gaps"].toInt();
    assessment["currentCoverage"]=total?int(100.0*totals["currents"].toInt()/total):0;
    assessment["voltageCoverage"]=total?int(100.0*totals["voltages"].toInt()/total):0;
    assessment["temperatureCoverage"]=total?int(100.0*totals["temps"].toInt()/total):0;
    assessment["historyText"]=QString::fromUtf8("%1 个采样 · %2 个断点").arg(total).arg(assessment["gapCount"].toInt());
    assessment["period"]=timeText(totals["first_ms"].toLongLong())+" / "+timeText(totals["last_ms"].toLongLong());
    assessment["storageText"]=QString::number(QFileInfo(directory+"/history.sqlite").size()/1048576.0,'f',2)+" MB";
    QVariantMap change=first(db.query("SELECT utc_ms FROM samples WHERE source_changed=1 ORDER BY id DESC LIMIT 1"));
    assessment["sourceAge"]=change.isEmpty()?QString::fromUtf8("未知"):duration(qMax(qint64(0),s["utc_ms"].toLongLong()-change["utc_ms"].toLongLong())/1000.0);
    assessment["missingCapacity"]=s["full_capacity"].isNull()||s["remaining"].isNull();
    QStringList missing;if(s["full_capacity"].isNull())missing<<QString::fromUtf8("满充容量");
    if(s["remaining"].isNull())missing<<QString::fromUtf8("剩余容量");
    if(s["input_current"].isNull())missing<<QString::fromUtf8("输入电流");
    assessment["missingText"]=missing.isEmpty()?QString::fromUtf8("主要字段均有效"):missing.join(QString::fromUtf8("、"))+QString::fromUtf8("未提供");
    QVariantMap active=first(db.query("SELECT * FROM sessions WHERE end_ms IS NULL ORDER BY id DESC LIMIT 1"));
    QVariantMap activeFormatted=formatSession(active);
    current["sessionText"]=activeFormatted.value("shortSummary",QString::fromUtf8("尚无区间"));
    current["sessionCoverage"]=activeFormatted.value("coverageText",QString::fromUtf8("待积累"));
    current["sessionDuration"]=activeFormatted.value("durationText",QString());
    current["sessionId"]=active.value("id",0);
    current["sessionActive"]=!active.isEmpty()&&fresh&&!isPaused;
    updateCapacity(s);
    QVariantMap sign=first(db.query("SELECT SUM(ready=1 AND mode='discharge' AND current < -2) AS discharges,"
        "SUM(ready=1 AND mode='charge' AND current > 2) AS charges FROM samples"));
    bool dischargeVerified=sign["discharges"].toInt()>0,chargeVerified=sign["charges"].toInt()>0;
    assessment["directionText"]=dischargeVerified&&chargeVerified?QString::fromUtf8("充、放电方向已有观测"):
        dischargeVerified?QString::fromUtf8("已观测负电流放电；充电待验证"):
        chargeVerified?QString::fromUtf8("已观测正电流充电；放电待验证"):QString::fromUtf8("尚无非零充放电电流观测");
    if(!fresh)assessment["summary"]=QString::fromUtf8("采集器心跳已过期，当前值是历史读数");
    else if(isPaused)assessment["summary"]=QString::fromUtf8("采集已暂停，暂停区间不计入积分");
    else if(!s["ready"].toBool())assessment["summary"]=QString::fromUtf8("电池数据未就绪，数值不参与评估");
    else if(s["current"].isNull())assessment["summary"]=QString::fromUtf8("电池电流未提供，容量积分不可用");
    else if(s["mode"]=="plugged")assessment["summary"]=QString::fromUtf8("接电静置；尚无可用于容量评估的放电区间");
    else assessment["summary"]=QString::fromUtf8("正在记录%1；容量结论取决于有效覆盖与电量跨度").arg(Battery::modeLabel(s["mode"].toString()));
    message=state["error"].toString().isEmpty()?current["collectorText"].toString():QString::fromUtf8("采集异常：")+state["error"].toString();
    if(id!=sampleId){sampleId=id;updateCharts();updateRecords();if(detailId)selectSession(detailId);}
    QVariantMap newest=first(db.query("SELECT * FROM events ORDER BY id DESC LIMIT 1"));
    qint64 eventId=newest.value("id",0).toLongLong();
    if(eventId!=lastEventId){
        QVariantList recent=db.query("SELECT * FROM events ORDER BY id DESC LIMIT 100");
        QVariantList items;
        foreach(const QVariant &v,recent){
            QVariantMap r=v.toMap();r["title"]=r["text"];r["description"]=timeText(r["utc_ms"].toLongLong());
            r["image"]="asset:///icons/"+QString(r["kind"].toString().contains("soc")||r["kind"]=="high-temperature"?"info":"battery")+".png";
            items<<r;
        }
        eventRecords->clear();eventRecords->append(items);
        QString kind=newest["kind"].toString();
        if(lastEventId>=0&&(kind=="low-soc"||kind=="high-soc"||kind=="high-temperature")&&
            Battery::utcMillis()-newest["utc_ms"].toLongLong()<60000)emit alerted(newest["text"].toString());
        lastEventId=eventId;
    }
    emit changed();
}
void Backend::updateCapacity(const QVariantMap &sample){
    const bool ready=sample["ready"].toBool();
    QVariant remaining=ready?Battery::remainingCapacityEstimate(sample["remaining"],sample["soc"]):QVariant();
    QVariant health=ready?Battery::healthCapacityEstimate(sample["design"],sample["health"]):QVariant();
    capacityValues["reportedText"]=sample["full_capacity"].isNull()?QString("--"):metric(sample["full_capacity"],0);
    capacityValues["designText"]=sample["design"].isNull()?QString("--"):metric(sample["design"],0);
    capacityValues["remainingText"]=remaining.isValid()?metric(remaining,0):QString("--");
    capacityValues["healthText"]=health.isValid()?metric(health,0):QString("--");
    capacityValues["remainingNote"]=remaining.isValid()?QString::fromUtf8("剩余电量 / SOC"):
        !sample["soc"].isNull()&&sample["soc"].toDouble()<20?QString::fromUtf8("SOC 低于 20%"):QString::fromUtf8("缺少有效读数");
    capacityValues["healthNote"]=health.isValid()?QString::fromUtf8("设计容量 × 健康度"):QString::fromUtf8("缺少有效读数");
    const qint64 session=current["sessionId"].toLongLong();const int battery=sample["battery_id"].toInt();
    if(session==capacitySessionId&&battery==capacityBatteryId)return;
    capacitySessionId=session;capacityBatteryId=battery;
    const char *modes[]={"discharge","charge"};
    for(int i=0;i<2;++i){
        QString mode=modes[i];QVariantMap row=db.capacitySession(mode,battery);
        QVariantMap formatted=formatSession(row,true);
        capacityValues[mode+"Text"]=formatted.value("estimateValue",QString("--"));
        capacityValues[mode+"Ready"]=formatted.value("estimateReady",false);
        capacityValues[mode+"Note"]=row.isEmpty()?QString::fromUtf8("待完整区间"):
            QString::fromUtf8("%1 · %2% 跨度").arg(timeText(row["last_ms"].toLongLong(),"MM-dd"))
            .arg(qAbs(row["start_soc"].toInt()-row["end_soc"].toInt()));
        capacityValues[mode+"SessionId"]=row.value("id",0);
    }
}
QVariantMap Backend::formatSession(const QVariantMap &row,bool capacityVerified)const{
    QVariantMap r=row;if(row.isEmpty())return r;
    QString mode=row["mode"].toString();bool closed=!row["end_ms"].isNull();
    double elapsed=row["elapsed_s"].toDouble(),coverage=elapsed>0?row["covered_s"].toDouble()/elapsed:0;
    r["title"]=Battery::modeLabel(mode)+" · "+timeText(row["start_ms"].toLongLong(),"MM-dd HH:mm");
    r["timeText"]=timeText(row["start_ms"].toLongLong())+" / "+timeText(row["last_ms"].toLongLong());
    r["status"]=closed?duration(elapsed):QString::fromUtf8("进行中");
    r["durationText"]=duration(elapsed);
    r["socText"]=metric(row["start_soc"],0,"%")+QString::fromUtf8(" → ")+metric(row["end_soc"],0,"%");
    const bool hasCharge=row["covered_s"].toDouble()>0&&(mode=="charge"||mode=="discharge");
    r["chargeText"]=hasCharge?QString::number(row["mah"].toDouble(),'f',2)+" mAh":QString::fromUtf8("尚无有效积分");
    r["energyText"]=row["energy_covered_s"].toDouble()>0?QString::number(row["mwh"].toDouble(),'f',2)+" mWh":QString::fromUtf8("未提供");
    r["coverageText"]=mode=="plugged"||mode=="unknown"?QString::fromUtf8("不适用"):
        elapsed>0?QString::number(coverage*100,'f',1)+"%":QString::fromUtf8("待积累");
    r["shortSummary"]=r["socText"].toString()+" · "+r["chargeText"].toString();
    r["description"]=r["shortSummary"].toString();
    QVariant cap=capacityVerified?Battery::capacityEstimate(mode,row["mah"].toDouble(),coverage,
        row["start_soc"].isNull()?-1:row["start_soc"].toInt(),row["end_soc"].isNull()?-1:row["end_soc"].toInt(),
        closed,row["stable_soc"].toBool()):QVariant();
    if(mode=="charge"&&capacityVerified)cap=Battery::chargeCapacityEstimate(row["mah"].toDouble(),coverage,
        row["start_soc"].isNull()?-1:row["start_soc"].toInt(),row["end_soc"].isNull()?-1:row["end_soc"].toInt(),
        closed,row["stable_soc"].toBool());
    r["estimateValue"]=cap.isValid()?metric(cap,0):QString("--");
    r["estimateKind"]=QString::fromUtf8(mode=="charge"?"充电积分外推":"放电积分外推");
    r["estimateText"]=cap.isValid()?metric(cap,0," mAh"):QString::fromUtf8("条件不足");
    r["estimateReady"]=cap.isValid();
    r["evaluation"]=cap.isValid()?QString::fromUtf8("覆盖 %1% · 跨度 %2%").arg(coverage*100,0,'f',1)
        .arg(qAbs(row["start_soc"].toInt()-row["end_soc"].toInt())):
        mode!="discharge"&&mode!="charge"?QString::fromUtf8("无充放电积分"):
        !closed?QString::fromUtf8("区间进行中"):
        !row["stable_soc"].toBool()?QString::fromUtf8("SOC 回跳"):
        coverage<0.95?QString::fromUtf8("覆盖不足 95%"):
        QString::fromUtf8("数据条件不足");
    r["image"]="asset:///icons/"+QString(mode=="charge"?"charge":mode=="discharge"?"discharge":mode=="plugged"?"battery":"info")+".png";
    r["samplesText"]=QString::number(row["sample_count"].toInt());
    return r;
}
void Backend::updateRecords(){
    records->clear();
    QVariantList bind;QString condition;
    if(!sessionFilter.isEmpty()){condition=" WHERE mode=?";bind<<sessionFilter;}
    QVariantList rows=db.query("SELECT * FROM sessions"+condition+" ORDER BY id DESC LIMIT 200",bind);
    QVariantList formatted;foreach(const QVariant &row,rows)formatted<<formatSession(row.toMap());
    records->append(formatted);
}
void Backend::updateCharts(){
    qint64 now=Battery::utcMillis();
    QVariantMap overviewStats;QVariantList latest=db.history(now-3600000,now,&overviewStats);
    overview=chart(latest,"soc",154,QString(),&overviewLabels,now-3600000,now);
    if(loadedGeneration!=historyGeneration){
        points.clear();markers.clear();historyStats.clear();
        rangeEnd=endTime?endTime:now;rangeBegin=rangeEnd-qint64(hours)*3600000;formatHistory();
        historyStats["loading"]=true;historyStats["summaryText"]=QString::fromUtf8("正在读取历史");
        soc=chart(points,"soc",228,QString(),&socLabels,rangeBegin,rangeEnd);
        other=chart(points,channel,228,QString(),&otherLabels,rangeBegin,rangeEnd);
        socLabels["empty"]=QString::fromUtf8("正在读取历史");otherLabels["empty"]=socLabels["empty"];
    }
    if(historyBusy){historyPending=true;emit chartsChanged();return;}
    if(loadedGeneration==historyGeneration&&(endTime||Battery::monoMillis()<nextHistoryRefresh)){
        emit chartsChanged();return;
    }
    historyLoader->path=directory+"/history.sqlite";historyLoader->end=endTime?endTime:now;
    historyLoader->begin=historyLoader->end-qint64(hours)*3600000;historyLoader->generation=historyGeneration;
    loadedGeneration=historyGeneration;historyPending=false;historyBusy=true;
    nextHistoryRefresh=Battery::monoMillis()+(hours>=168?60000:hours>=24?30000:hours>=6?10000:0);
    historyLoader->start();
    emit chartsChanged();
}
void Backend::historyReady(){
    historyLoader->wait();historyBusy=false;
    if(historyLoader->generation==historyGeneration){
        rangeBegin=historyLoader->begin;rangeEnd=historyLoader->end;
        historyStats=historyLoader->summary;points=historyLoader->points;markers=historyLoader->events;formatHistory();
        historyStats["loading"]=false;
        inspectionCacheValid=false;inspectionSamples.clear();
        if(inspectionPending){++inspectionGeneration;inspectionTimer.start(33);}
        soc=chart(points,"soc",228,QString(),&socLabels,rangeBegin,rangeEnd,markers);
        other=chart(points,channel,228,QString(),&otherLabels,rangeBegin,rangeEnd,markers);
        emit chartsChanged();emit changed();emit inspectionChanged();
    }else historyPending=true;
    if(historyPending)updateCharts();
}
void Backend::formatHistory(){
    historyStats["beginMs"]=rangeBegin;historyStats["endMs"]=rangeEnd;historyStats["displayPoints"]=points.size();
    historyStats["rangeText"]=timeText(rangeBegin)+"  /  "+timeText(rangeEnd);
    historyStats["coverageText"]=QString::number(historyStats["coverage"].toDouble(),'f',1)+"%";
    historyStats["summaryText"]=QString::fromUtf8("%1 个采样 · 覆盖 %2 · %3 个断点")
        .arg(historyStats["samples"].toInt()).arg(historyStats["coverageText"].toString()).arg(historyStats["gaps"].toInt());
    historyStats["chargeText"]=metric(historyStats["chargeMah"],1," mAh");
    if(!historyStats.value("complete",false).toBool())historyStats["summaryText"]=QString::fromUtf8("历史读取不完整，统计暂不可用");
    historyStats["dischargeText"]=metric(historyStats["dischargeMah"],1," mAh");
    const char *names[]={"current","voltage","temperature"};
    for(int i=0;i<3;++i){
        QString n=names[i];double scale=i==1?1000.0:1.0;QString unit=i==0?" mA":i==1?" V":" C";
        QVariant mean=historyStats[n+"Mean"],lo=historyStats[n+"Min"],hi=historyStats[n+"Max"];
        historyStats[n+"MeanText"]=mean.isValid()?metric(mean.toDouble()/scale,i==0?0:i==1?3:1,unit):QString::fromUtf8("未提供");
        historyStats[n+"RangeText"]=lo.isValid()?metric(lo.toDouble()/scale,i==0?0:i==1?3:1)+" / "+
            metric(hi.toDouble()/scale,i==0?0:i==1?3:1,unit):QString::fromUtf8("未提供");
    }
}
bb::cascades::Image Backend::chart(const QVariantList &rows,const QString &field,int height,const QString &save,QVariantMap *labels,
    qint64 begin,qint64 end,const QVariantList &events){
    ++chartRenderCount;
    const int width=720,left=76,right=20,top=28,bottom=38;
    QImage bitmap(width,height,QImage::Format_ARGB32_Premultiplied);bitmap.fill(QColor("#101820").rgba());
    QPainter p(&bitmap);p.setRenderHint(QPainter::Antialiasing);
    QRectF plot(left,top,width-left-right,height-top-bottom);
    if(!begin)begin=rows.isEmpty()?Battery::utcMillis()-60000:rows.first().toMap()["utc_ms"].toLongLong();
    if(!end)end=rows.isEmpty()?Battery::utcMillis():rows.last().toMap()["utc_ms"].toLongLong();
    if(end<=begin){begin-=10000;end+=10000;}
    double low=1e30,high=-1e30;int valid=0;
    foreach(const QVariant &v,rows){QVariant value=v.toMap()[field];if(value.isValid()&&!value.isNull()){low=qMin(low,value.toDouble());high=qMax(high,value.toDouble());++valid;}}
    if(!valid){low=0;high=1;}
    else if(field=="soc"){double pad=qMax(2.0,(high-low)*0.15);low=qMax(0.0,low-pad);high=qMin(100.0,high+pad);if(high<=low)low=high-2;}
    else {double pad=qMax(field=="current"?20.0:field=="voltage"?15.0:0.5,(high-low)*0.15);low-=pad;high+=pad;}
    // Cascades owns text rendering; QtGui's font database is not initialized here.
    QVariantMap axes;axes["height"]=height;
    axes["unit"]=field=="soc"?"%":field=="current"?"mA":field=="voltage"?"V":"C";
    axes["empty"]=valid?QString():rows.isEmpty()?QString::fromUtf8("尚无采样记录"):QString::fromUtf8("此参数没有有效读数");
    const char *levels[]={"high","middle","low"};
    const char *times[]={"start","center","end"};
    p.setPen(QColor("#34383b"));
    for(int i=0;i<3;++i){
        double y=plot.top()+plot.height()*i/2.0;p.drawLine(QPointF(plot.left(),y),QPointF(plot.right(),y));
        double value=high-(high-low)*i/2.0;
        axes[levels[i]]=!valid?QString():field=="voltage"?QString::number(value/1000,'f',2):QString::number(value,'f',field=="temperature"?1:0);
    }
    for(int i=0;i<3;++i){
        qint64 ts=begin+(end-begin)*i/2;
        axes[times[i]]=timeText(ts,end-begin>=86400000?"MM-dd":"HH:mm");
    }
    if(labels)*labels=axes;
    p.save();p.setClipRect(plot.adjusted(-3,-3,3,8));
    QVariantMap previous;bool previousValid=false;
    foreach(const QVariant &v,rows){
        QVariantMap row=v.toMap();qint64 ts=row["utc_ms"].toLongLong();
        double x=plot.left()+double(ts-begin)/(end-begin)*plot.width();
        bool present=row["ready"].toBool()&&row[field].isValid()&&!row[field].isNull();
        bool continuous=previousValid&&row["gap_reason"].toString().isEmpty()&&!previous["display_break"].toBool()&&
            row["run_id"]==previous["run_id"]&&row["session_id"]==previous["session_id"]&&ts>previous["utc_ms"].toLongLong()&&
            row["mono_ms"].toLongLong()>previous["mono_ms"].toLongLong()&&
            ((row["display_decimated"].toBool()&&previous["display_decimated"].toBool())||
            ts-previous["utc_ms"].toLongLong()<=qMax(row["interval_s"].toInt(),previous["interval_s"].toInt())*3000);
        if(present){
            double y=plot.bottom()-(row[field].toDouble()-low)/(high-low)*plot.height();
            p.setPen(QPen(modeColor(row["mode"].toString()),3));
            if(continuous){
                double px=plot.left()+double(previous["utc_ms"].toLongLong()-begin)/(end-begin)*plot.width();
                double py=plot.bottom()-(previous[field].toDouble()-low)/(high-low)*plot.height();
                p.drawLine(QPointF(px,py),QPointF(x,y));
            } else {p.setBrush(modeColor(row["mode"].toString()));p.drawEllipse(QPointF(x,y),3,3);p.setBrush(Qt::NoBrush);}
        }
        if(continuous){
            double px=plot.left()+double(previous["utc_ms"].toLongLong()-begin)/(end-begin)*plot.width();
            p.fillRect(QRectF(px,plot.bottom()+3,qMax(1.0,x-px),4),modeColor(previous["mode"].toString()));
        }
        if(!row["gap_reason"].toString().isEmpty()&&row["gap_reason"]!="display-bucket-break"){
            p.setPen(QPen(QColor("#f09b87"),1,Qt::DashLine));p.drawLine(QPointF(x,plot.top()),QPointF(x,plot.bottom()));
        }
        previous=row;previousValid=present;
    }
    p.restore();
    foreach(const QVariant &v,events){
        QVariantMap e=v.toMap();double x=plot.left()+double(e["utc_ms"].toLongLong()-begin)/(end-begin)*plot.width();
        QString kind=e["kind"].toString();
        QColor color=kind.startsWith("power-")?QColor("#efc56a"):kind.startsWith("mode-")?modeColor(kind.mid(5)):QColor("#f09b87");
        p.setPen(QPen(color,2));p.drawLine(QPointF(x,plot.top()-10),QPointF(x,plot.top()-3));
    }
    p.end();if(!save.isEmpty())bitmap.save(save);
    QImage rgba=bitmap.rgbSwapped();
    return bb::cascades::Image(bb::ImageData::fromPixels(rgba.constBits(),bb::PixelFormat::RGBA_Premultiplied,width,height,rgba.bytesPerLine()));
}
void Backend::setWindow(int value){
    if(value!=1&&value!=6&&value!=24&&value!=168&&value!=720)return;
    if(hours==value)return;hours=value;++historyGeneration;clearInspection();updateCharts();emit changed();
}
void Backend::setParameter(const QString &value){
    if(value!="current"&&value!="voltage"&&value!="temperature"&&value!="none")return;
    if(channel==value)return;channel=value;other=chart(points,channel,228,QString(),&otherLabels,rangeBegin,rangeEnd,markers);
    emit changed();emit chartsChanged();
}
void Backend::moveWindow(int direction){
    if(direction!=-1&&direction!=1)return;
    qint64 now=Battery::utcMillis(),target=(endTime?endTime:now)+direction*qint64(hours)*3600000;
    endTime=target>=now?0:target;++historyGeneration;clearInspection();updateCharts();emit changed();
}
void Backend::browseDate(const QDateTime &date){
    if(!date.isValid())return;
    qint64 target=QDateTime(date.date(),QTime(23,59,59)).toMSecsSinceEpoch(),now=Battery::utcMillis();
    endTime=target>=now?0:target;++historyGeneration;clearInspection();updateCharts();emit changed();
}
void Backend::followLatest(){endTime=0;++historyGeneration;clearInspection();updateCharts();emit changed();}
void Backend::clearInspection(){
    inspectionTimer.stop();++inspectionGeneration;inspectionPending=false;inspectionCacheValid=false;
    inspectionSamples.clear();requestedInspectionTime=0;inspectionTime=0;pointText.clear();
    emit inspectionChanged();completeInspectionResponse();
}
void Backend::filterSessions(const QString &mode){
    if(mode!=""&&mode!="charge"&&mode!="discharge"&&mode!="plugged")return;
    sessionFilter=mode;updateRecords();emit changed();
}
void Backend::selectSession(qint64 id){
    QVariantMap row=first(db.query("SELECT * FROM sessions WHERE id=?",QVariantList()<<id));if(row.isEmpty())return;
    detailId=id;
    const bool verified=!db.capacitySession(row["mode"].toString(),row["battery_id"].toInt(),id).isEmpty();
    selected=formatSession(row,verified);
    QVariantMap stats;detailPoints=db.history(row["start_ms"].toLongLong(),row["last_ms"].toLongLong(),&stats,id);
    selected["currentMeanText"]=metric(stats["currentMean"],0," mA");
    selected["temperatureMaxText"]=metric(stats["temperatureMax"],1," C");
    detailSoc=chart(detailPoints,"soc",228,QString(),&detailSocLabels);
    detailCurrent=chart(detailPoints,"current",228,QString(),&detailCurrentLabels);emit detailChanged();
}
void Backend::inspect(double x,double width,bool immediate){
    if(!qIsFinite(x)||!qIsFinite(width)||width<=0||rangeEnd<=rangeBegin||historyStats.value("loading",false).toBool())return;
    double ratio=qBound(0.0,(x/width*720.0-76)/624.0,1.0);
    qint64 target=rangeBegin+qint64((rangeEnd-rangeBegin)*ratio);
    if(target!=requestedInspectionTime||(!inspectionCacheValid&&!inspectionPending)){
        requestedInspectionTime=target;++inspectionGeneration;inspectionPending=true;
    }
    if(!inspectionPending)return;
    if(immediate)processInspection();
    else if(!inspectionTimer.isActive())inspectionTimer.start(33);
}
void Backend::processInspection(){
    inspectionTimer.stop();
    if(!inspectionPending)return;
    qint64 target=requestedInspectionTime;
    // Reuse the same two raw neighbours for targets between their timestamps.
    if(inspectionCacheValid&&(target==cachedInspectionTarget||
        (target>cachedInspectionBegin&&target<cachedInspectionEnd))){
        inspectionPending=false;resolveInspection(target,inspectionSamples);completeInspectionResponse();return;
    }
    if(inspectionTime!=target){inspectionTime=target;emit inspectionChanged();}
    if(inspectionBusy)return;
    inspectionLoader->path=directory+"/history.sqlite";inspectionLoader->begin=rangeBegin;inspectionLoader->end=rangeEnd;
    inspectionLoader->target=target;inspectionLoader->generation=inspectionGeneration;
    inspectionBusy=true;++inspectionLookupCount;inspectionLoader->start();
}
void Backend::inspectionReady(){
    inspectionLoader->wait();inspectionBusy=false;
    bool sameRange=inspectionLoader->begin==rangeBegin&&inspectionLoader->end==rangeEnd;
    if(sameRange&&inspectionLoader->error.isEmpty()){
        inspectionSamples=inspectionLoader->nearby;inspectionCacheValid=true;
        cachedInspectionTarget=inspectionLoader->target;cachedInspectionBegin=rangeBegin;cachedInspectionEnd=rangeEnd;
        foreach(const QVariant &v,inspectionSamples){
            qint64 ts=v.toMap()["utc_ms"].toLongLong();
            if(ts<=cachedInspectionTarget)cachedInspectionBegin=ts;
            if(ts>=cachedInspectionTarget)cachedInspectionEnd=ts;
        }
    }
    if(sameRange&&inspectionPending&&inspectionLoader->generation==inspectionGeneration){
        inspectionPending=false;resolveInspection(inspectionLoader->target,inspectionLoader->nearby,inspectionLoader->error);
        completeInspectionResponse();
    }else if(inspectionPending&&!inspectionTimer.isActive())processInspection();
}
void Backend::resolveInspection(qint64 target,const QVariantList &nearby,const QString &error){
    QVariantMap closest;qint64 distance=Q_INT64_C(9223372036854775807);
    foreach(const QVariant &v,nearby){QVariantMap row=v.toMap();qint64 d=qAbs(row["utc_ms"].toLongLong()-target);if(d<distance){distance=d;closest=row;}}
    qint64 time;QString text;
    if(!error.isEmpty()){
        time=target;text=timeText(target)+" · "+QString::fromUtf8("采样读取失败");
    }else if(closest.isEmpty()||distance>qMax(5000,closest["interval_s"].toInt()*1500)){
        time=target;text=timeText(target)+" · "+QString::fromUtf8("此时无采样");
    }else{
        time=closest["utc_ms"].toLongLong();
        text=timeText(time)+" · "+metric(closest["soc"],0,"%")+" · "+
            metric(closest["current"],0," mA")+"\n"+Battery::modeLabel(closest["mode"].toString())+" · "+
            metric(closest["voltage"].isNull()?QVariant():QVariant(closest["voltage"].toDouble()/1000),3," V")+" · "+metric(closest["temperature"],1," C");
    }
    if(time!=inspectionTime||text!=pointText){inspectionTime=time;pointText=text;emit inspectionChanged();}
}
void Backend::completeInspectionResponse(){
    if(inspectionResponse.isEmpty())return;
    inspectionResponse["inspection"]=pointText;inspectionResponse["cursor"]=inspectionCursor();
    if(inspectionResponse["op"]=="inspect-benchmark"){
        inspectionResponse["settleMs"]=Battery::monoMillis()-inspectionBenchmarkStarted;
        inspectionResponse["lookupCount"]=inspectionLookupCount-inspectionBenchmarkLookups;
    }
    writeDiagnosticResponse(inspectionResponse);inspectionResponse.clear();
}
bool Backend::saveSettings(int interval,bool paused){
    QSettings settings(directory+"/settings.ini",QSettings::IniFormat);
    settings.setValue("interval",interval);settings.setValue("paused",paused);settings.sync();
    return settings.status()==QSettings::NoError;
}
void Backend::configure(int interval,bool paused){
    if(interval!=5&&interval!=10&&interval!=30&&interval!=60)return;
    if(!saveSettings(interval,paused)){message=QString::fromUtf8("采集设置保存失败");emit changed();return;}
    sampleInterval=interval;isPaused=paused;message=QString::fromUtf8("设置已保存，等待采集器确认");emit changed();
}
void Backend::configureAlerts(bool enabled,int low,int high,int temperature){
    if(low<5||low>40||high<60||high>100||temperature<35||temperature>55)return;
    QSettings settings(directory+"/settings.ini",QSettings::IniFormat);
    settings.setValue("alerts/enabled",enabled);settings.setValue("alerts/low",low);
    settings.setValue("alerts/high",high);settings.setValue("alerts/temperature",temperature);settings.sync();
    if(settings.status()!=QSettings::NoError){emit exported(QString::fromUtf8("提醒设置保存失败"));return;}
    refresh();
}
void Backend::exportData(bool selectedSession){
    QString dir=QDir::currentPath()+"/shared/documents/BBattery";
    if(!QDir().mkpath(dir)){emit exported(QString::fromUtf8("导出目录不可写"));return;}
    QString base=dir+"/battery-"+QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss")+"-"+QUuid::createUuid().toString().mid(1,8);
    QString path=base+"-samples.csv";QFile file(path);if(!file.open(QIODevice::WriteOnly)){emit exported(QString::fromUtf8("导出失败"));return;}
    QTextStream stream(&file);stream.setCodec("UTF-8");stream.setGenerateByteOrderMark(true);
    const char *columns[]={"id","utc_ms","mono_ms","run_id","session_id","snapshot_id","interval_s","ready","battery_id","mode","charger","phase",
        "soc","current","voltage","temperature","design","full_capacity","remaining","health","cycles","input_limit","charge_limit","input_current","source_changed","gap_reason","error"};
    QStringList names;for(unsigned i=0;i<sizeof(columns)/sizeof(columns[0]);++i)names<<columns[i];stream<<names.join(",")<<"\n";
    QString sql="SELECT * FROM samples";if(selectedSession&&detailId)sql+=QString(" WHERE session_id=%1").arg(detailId);sql+=" ORDER BY id";
    sqlite3_stmt *s=0;int rc=sqlite3_prepare_v2(db.handle(),sql.toUtf8().constData(),-1,&s,0);
    bool ok=rc==SQLITE_OK;
    if(ok){while((rc=sqlite3_step(s))==SQLITE_ROW){
        QStringList values;for(int i=0;i<sqlite3_column_count(s);++i){
            const unsigned char *text=sqlite3_column_text(s,i);values<<csvValue(text?QVariant(QString::fromUtf8(reinterpret_cast<const char*>(text))):QVariant());
        }stream<<values.join(",")<<"\n";
    }ok=rc==SQLITE_DONE;}
    if(s)sqlite3_finalize(s);stream.flush();ok=ok&&stream.status()==QTextStream::Ok;file.close();
    bb::data::JsonDataAccess json;QVariantMap summary;summary["generatedUtc"]=QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    summary["current"]=current;summary["quality"]=assessment;summary["units"]="current=mA,voltage=mV,temperature=C,capacity=mAh,utc_ms=Unix UTC milliseconds";
    summary["missingValues"]="Empty CSV fields are unavailable, not zero";
    summary["capacity"]=capacityValues;
    summary["capacityMethod"]="Separate estimates: completed discharge/charge integrals, remaining mAh / SOC, design capacity * reported health; not calibrated full-capacity measurements";
    summary["selectedSession"]=selectedSession?selected:QVariantMap();summary["samplesFile"]=path;
    summary["viewedWindow"]=historyStats;
    summary["aggregationMethod"]="Time-weighted valid adjacent readings; raw-record extrema; gaps excluded; rendering decimation only";
    summary["events"]=db.query("SELECT * FROM events"+(selectedSession&&detailId?QString(" WHERE session_id=%1").arg(detailId):QString())+" ORDER BY id");
    json.save(summary,base+"-quality.json");ok=ok&&!json.hasError();
    emit exported(ok?QString::fromUtf8("已导出至 Documents/BBattery"):QString::fromUtf8("导出不完整，请检查存储"));
}
void Backend::diagnostic(){
    QFile request("data/diagnostics/request.json");if(!request.open(QIODevice::ReadOnly))return;
    bb::data::JsonDataAccess json;QVariantMap args=json.loadFromBuffer(request.readAll()).toMap();request.close();request.remove();
    QVariantMap response;response["requestId"]=args["requestId"];response["ok"]=true;
    QString operation=args["op"].toString();
    if(operation=="capture"){
        bb::system::Screenshot capture;
        QString path=capture.captureWindow(QUrl::fromLocalFile(QDir::currentPath()+"/data/diagnostics/ui.png"),
            bb::cascades::Application::instance()->mainWindow()->handle());
        response["ok"]=!path.isEmpty();response["path"]=path;response["error"]=int(capture.error());
    }else if(operation=="backup"){
        QString path=QDir::currentPath()+"/data/diagnostics/history.sqlite";sqlite3 *destination=0;
        bool ok=sqlite3_open(QFile::encodeName(path).constData(),&destination)==SQLITE_OK;
        if(ok){
            sqlite3_backup *copy=sqlite3_backup_init(destination,"main",db.handle(),"main");
            ok=copy!=0;int rc=copy?sqlite3_backup_step(copy,-1):SQLITE_ERROR;
            if(copy){int finish=sqlite3_backup_finish(copy);ok=ok&&rc==SQLITE_DONE&&finish==SQLITE_OK;}
        }
        if(destination)sqlite3_close(destination);
        response["ok"]=ok;response["path"]=path;
    }else if(operation=="view"){
        QVariant ignored;response["ok"]=scene&&QMetaObject::invokeMethod(scene,"testView",Q_RETURN_ARG(QVariant,ignored),Q_ARG(QVariant,QVariant(args)));
        response["layout"]=ignored;
    }else if(operation=="state"){
        refresh();response["live"]=current;response["quality"]=assessment;response["history"]=historyStats;
        response["capacity"]=capacityValues;
        response["following"]=following();response["alerts"]=alertSettings;response["events"]=eventRecords->size();
        response["rows"]=records->size();response["inspection"]=pointText;response["socAxes"]=socLabels;
        response["inspectionCursor"]=inspectionCursor();response["chartRenders"]=chartRenderCount;
        response["parameterAxes"]=otherLabels;response["euid"]=qint64(geteuid());
    }
    else if(operation=="chart-export"){
        chart(points,"soc",228,"data/diagnostics/soc.png",0,rangeBegin,rangeEnd,markers);
        chart(points,channel,228,"data/diagnostics/parameter.png",0,rangeBegin,rangeEnd,markers);
        response["soc"]=QDir::currentPath()+"/data/diagnostics/soc.png";response["parameter"]=QDir::currentPath()+"/data/diagnostics/parameter.png";
    }else if(operation=="select-session"){selectSession(args["id"].toLongLong());response["detail"]=selected;}
    else if(operation=="configure"){configure(args["interval"].toInt(),args["paused"].toBool());}
    else if(operation=="configure-alerts"){configureAlerts(args["enabled"].toBool(),args["low"].toInt(),args["high"].toInt(),args["temperature"].toInt());}
    else if(operation=="inspect"){
        inspectionResponse=response;inspect(args["x"].toDouble(),args["width"].toDouble(),true);
        if(!inspectionPending)completeInspectionResponse();
        return;
    }
    else if(operation=="inspect-benchmark"){
        int count=qBound(1,args.value("count",120).toInt(),2000),renders=chartRenderCount;
        qint64 started=Battery::monoMillis();
        inspectionBenchmarkStarted=started;inspectionBenchmarkLookups=inspectionLookupCount;
        for(int i=0;i<count;++i)inspect(76.0+624.0*i/qMax(1,count-1),720);
        response["moves"]=count;response["elapsedMs"]=Battery::monoMillis()-started;
        response["chartRenders"]=chartRenderCount-renders;response["op"]=operation;
        inspectionResponse=response;processInspection();
        if(!inspectionPending)completeInspectionResponse();
        return;
    }
    else if(operation=="export"){exportData(false);}
    else {response["ok"]=false;response["error"]="Unknown finite diagnostic operation";}
    writeDiagnosticResponse(response);
}
void Backend::writeDiagnosticResponse(const QVariantMap &response){
    bb::data::JsonDataAccess json;QByteArray out;json.saveToBuffer(response,&out);QFile result("data/diagnostics/response.json");
    if(result.open(QIODevice::WriteOnly))result.write(out);
}
