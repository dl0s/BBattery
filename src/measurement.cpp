#include "measurement.h"
#include <QDateTime>
#include <QStringList>
#include <cmath>
#include <ctime>
#include <sys/pps.h>

namespace Battery {
Sample::Sample():utc(0),mono(0),batteryId(-1),interval(10),ready(false){}
bool Sample::has(const char *name) const { return values.value(name).isValid(); }
double Sample::value(const char *name) const { return values.value(name).toDouble(); }
Step::Step():seconds(0),charge(0),energy(0),chargeValid(false),energyValid(false){}
qint64 utcMillis(){ return QDateTime::currentMSecsSinceEpoch(); }
qint64 monoMillis(){
    struct timespec t;
    if(clock_gettime(CLOCK_MONOTONIC,&t)!=0) return -1;
    return qint64(t.tv_sec)*1000+t.tv_nsec/1000000;
}
QVariant number(double value,double low,double high){
    if(value!=value || value==80000000.0 || value<low || value>high) return QVariant();
    return QVariant(value);
}
QString modeFor(const Sample &s){
    if(!s.ready) return "unknown";
    if(s.has("current") && s.value("current") < -2) return "discharge";
    if(s.has("current") && s.value("current") > 2) return "charge";
    if(s.charger=="NONE") return "discharge";
    if(s.charger=="CHARGING" && s.phase!="NC" && s.phase!="DONE") return "charge";
    if(s.charger=="PLUGGED" || s.charger=="CHARGING") return "plugged";
    return "unknown";
}
Step integrate(const Sample &a,const Sample &b){
    Step r;
    const double dt=(b.mono-a.mono)/1000.0;
    if(a.run!=b.run) r.breakReason="collector-restart";
    else if(a.batteryId!=b.batteryId) r.breakReason="battery-changed";
    else if(dt<=0) r.breakReason="monotonic-reset";
    else if(std::fabs(double((b.utc-a.utc)-(b.mono-a.mono)))>2000) r.breakReason="clock-change";
    else if(dt>3*qMax(a.interval,b.interval)) r.breakReason="sampling-gap";
    if(!r.breakReason.isEmpty()) return r;
    r.seconds=dt;
    if(a.mode!=b.mode || (b.mode!="charge" && b.mode!="discharge") ||
       !a.ready || !b.ready || !a.has("current") || !b.has("current")) return r;
    const double ia=a.value("current"),ib=b.value("current");
    if((b.mode=="charge" && (ia<0 || ib<0)) ||
       (b.mode=="discharge" && (ia>0 || ib>0))) return r;
    r.chargeValid=true;
    r.charge=(std::fabs(ia)+std::fabs(ib))*0.5*dt/3600.0;
    if(a.has("voltage") && b.has("voltage")){
        r.energyValid=true;
        r.energy=(std::fabs(ia)*a.value("voltage")+std::fabs(ib)*b.value("voltage"))*0.5*dt/3600000.0;
    }
    return r;
}
QVariant capacityEstimate(const QString &mode,double q,double coverage,int start,int end,bool closed,bool stable){
    if(mode!="discharge" || !closed || !stable || coverage<0.95 ||
       start<0 || end<0 || start-end<30 || q<=0) return QVariant();
    return number(q*100.0/(start-end),1,20000);
}
QString modeLabel(const QString &mode){
    if(mode=="charge") return QString::fromUtf8("充电");
    if(mode=="discharge") return QString::fromUtf8("放电");
    if(mode=="plugged") return QString::fromUtf8("接电静置");
    return QString::fromUtf8("状态未知");
}
bool connected(const QString &charger){return charger=="PLUGGED"||charger=="CHARGING";}
QVariantMap sourceDetails(const QString &raw){
    QVariantMap result;QByteArray bytes=raw.toUtf8();pps_decoder_t d;
    if(pps_decoder_initialize(&d,bytes.data())!=PPS_DECODER_OK){pps_decoder_cleanup(&d);return result;}
    if(pps_decoder_push_object(&d,0)!=PPS_DECODER_OK){pps_decoder_cleanup(&d);return result;}
    pps_decoder_state_t root;pps_decoder_get_state(&d,&root);
    int count=-1,index=-1;const char *text=0;
    if(pps_decoder_push_object(&d,"ChargerInfo")==PPS_DECODER_OK){
        if(pps_decoder_get_int(&d,"NumOfSource",&count)==PPS_DECODER_OK&&count==0)result["charger"]="NONE";
        else if(count>0&&pps_decoder_get_int(&d,"ActiveCharger",&index)==PPS_DECODER_OK&&index>=0&&index<count&&
            pps_decoder_push_array(&d,"ChargerParams")==PPS_DECODER_OK&&pps_decoder_goto_index(&d,index)==PPS_DECODER_OK&&
            pps_decoder_push_object(&d,0)==PPS_DECODER_OK&&pps_decoder_get_string(&d,"ChargerStatus",&text)==PPS_DECODER_OK){
            QString status=QString::fromUtf8(text);
            if(status=="NONE"||status=="PLUGGED"||status=="CHARGING"||status=="BAD"||status=="ERROR")result["charger"]=status;
        }
    }
    pps_decoder_set_state(&d,&root);pps_decoder_status(&d,true);
    if(pps_decoder_push_object(&d,"SystemInfo")==PPS_DECODER_OK){
        int value;
        if(pps_decoder_get_int(&d,"ChargeCurrent",&value)==PPS_DECODER_OK)
            result["systemChargeCurrent"]=number(value,0,10000);
    }
    pps_decoder_set_state(&d,&root);pps_decoder_status(&d,true);
    if(pps_decoder_push_object(&d,"BatteryInfo")==PPS_DECODER_OK&&pps_decoder_get_int(&d,"ActiveBattery",&index)==PPS_DECODER_OK&&
        index>=0&&pps_decoder_push_array(&d,"BatteryStatus")==PPS_DECODER_OK&&index<pps_decoder_length(&d)&&
        pps_decoder_goto_index(&d,index)==PPS_DECODER_OK&&pps_decoder_push_object(&d,0)==PPS_DECODER_OK){
        const char *names[]={"TimeToEmpty","TimeToFull"};const char *keys[]={"timeToEmpty","timeToFull"};
        for(int i=0;i<2;++i){int value;if(pps_decoder_get_int(&d,names[i],&value)==PPS_DECODER_OK)result[keys[i]]=number(value,0,65534);}
    }
    pps_decoder_cleanup(&d);return result;
}
namespace {
Sample fromRow(const QVariantMap &row){
    Sample s;s.utc=row["utc_ms"].toLongLong();s.mono=row["mono_ms"].toLongLong();
    s.run=row["run_id"].toString();s.batteryId=row["battery_id"].toInt();
    s.ready=row["ready"].toBool();s.interval=row["interval_s"].toInt();s.mode=row["mode"].toString();
    const char *names[]={"current","voltage","temperature"};
    for(int i=0;i<3;++i)if(row[names[i]].isValid()&&!row[names[i]].isNull())s.values[names[i]]=row[names[i]];
    return s;
}
}
HistorySummary::HistorySummary(qint64 a,qint64 b):begin(a),end(b),count(0),gaps(0),covered(0),
    charge(0),discharge(0),chargeEnergy(0),dischargeEnergy(0),energySeconds(0),integrationSeconds(0){
    for(int i=0;i<3;++i){sums[i]=seconds[i]=0;minimums[i]=1e30;maximums[i]=-1e30;valid[i]=0;}
}
void HistorySummary::add(const QVariantMap &row){
    const char *fields[]={"current","voltage","temperature"};
    qint64 ts=row["utc_ms"].toLongLong();
    if(ts>=begin&&ts<=end){
        ++count;if(!row["gap_reason"].toString().isEmpty())++gaps;
        for(int i=0;i<3;++i)if(row["ready"].toBool()&&row[fields[i]].isValid()&&!row[fields[i]].isNull()){
            double v=row[fields[i]].toDouble();++valid[i];minimums[i]=qMin(minimums[i],v);maximums[i]=qMax(maximums[i],v);
        }
    }
    if(!previous.isEmpty()){
        Sample a=fromRow(previous),b=fromRow(row);Step step=integrate(a,b);
        qint64 left=qMax(begin,a.utc),right=qMin(end,b.utc);
        if(right>left&&b.utc>a.utc&&step.breakReason.isEmpty()&&row["gap_reason"].toString().isEmpty()&&a.ready&&b.ready){
            double f0=double(left-a.utc)/(b.utc-a.utc),f1=double(right-a.utc)/(b.utc-a.utc);
            double dt=step.seconds*(f1-f0);covered+=dt;
            for(int i=0;i<3;++i)if(a.has(fields[i])&&b.has(fields[i])){
                double x=a.value(fields[i]),y=b.value(fields[i]);
                sums[i]+=(x+(y-x)*(f0+f1)/2)*dt;seconds[i]+=dt;
            }
            if(step.chargeValid&&row["session_id"]==previous["session_id"]){
                double ia=std::fabs(a.value("current")),ib=std::fabs(b.value("current"));
                double q=(ia+(ib-ia)*(f0+f1)/2)*dt/3600;
                if(b.mode=="charge")charge+=q;else discharge+=q;
                integrationSeconds+=dt;
                if(step.energyValid){
                    double pa=ia*a.value("voltage"),pb=ib*b.value("voltage");
                    double e=(pa+(pb-pa)*(f0+f1)/2)*dt/3600000;
                    if(b.mode=="charge")chargeEnergy+=e;else dischargeEnergy+=e;
                    energySeconds+=dt;
                }
            }
        }
    }
    previous=row;
}
QVariantMap HistorySummary::result()const{
    QVariantMap r;double span=qMax(0.0,(end-begin)/1000.0);
    r["samples"]=count;r["gaps"]=gaps;r["coveredSeconds"]=covered;
    r["coverage"]=span>0?qBound(0.0,100*covered/span,100.0):0;
    r["chargeMah"]=integrationSeconds>0?QVariant(charge):QVariant();
    r["dischargeMah"]=integrationSeconds>0?QVariant(discharge):QVariant();
    r["chargeMwh"]=energySeconds>0?QVariant(chargeEnergy):QVariant();
    r["dischargeMwh"]=energySeconds>0?QVariant(dischargeEnergy):QVariant();
    r["integrationSeconds"]=integrationSeconds;r["energySeconds"]=energySeconds;
    const char *names[]={"current","voltage","temperature"};
    for(int i=0;i<3;++i){
        QString name=names[i];r[name+"Mean"]=seconds[i]>0?QVariant(sums[i]/seconds[i]):QVariant();
        r[name+"Min"]=valid[i]?QVariant(minimums[i]):QVariant();
        r[name+"Max"]=valid[i]?QVariant(maximums[i]):QVariant();
        r[name+"Valid"]=valid[i];r[name+"Seconds"]=seconds[i];
    }
    return r;
}
AlertMonitor::AlertMonitor():low(false),high(false),hot(false){}
QStringList AlertMonitor::evaluate(const Sample &s,const AlertConfig &c){
    QStringList events;
    if(!c.enabled){low=high=hot=false;return events;}
    if(!s.ready)return events;
    if(s.has("soc")){
        double soc=s.value("soc");
        if(soc>=c.lowSoc+5)low=false;
        if(soc<=c.highSoc-5)high=false;
        if(soc<=c.lowSoc&&!low){events<<"low-soc";low=true;}
        if(soc>=c.highSoc&&connected(s.charger)&&!high){events<<"high-soc";high=true;}
    }
    if(s.has("temperature")){
        double t=s.value("temperature");
        if(t<=c.temperature-3)hot=false;
        if(t>=c.temperature&&!hot){events<<"high-temperature";hot=true;}
    }
    return events;
}
bool selfTest(QString *report){
    QStringList passed;
#define CHECK(label,condition) do { if(!(condition)){*report="FAIL: " label;return false;} passed<<"PASS: " label; } while(0)
    CHECK("sentinel is missing",!number(80000000,0,90000000).isValid());
    CHECK("NaN is missing",!number(std::sqrt(-1.0),-100,100).isValid());
    CHECK("zero current remains valid",number(0,-10000,10000).isValid());
    Sample a,b;
    a.utc=100000;a.mono=100000;a.run="test";a.batteryId=244;a.ready=true;a.mode="discharge";
    a.values["current"]=-1000;a.values["voltage"]=4000;
    b=a;b.utc+=10000;b.mono+=10000;
    Step step=integrate(a,b);
    CHECK("trapezoidal mAh",step.chargeValid && std::fabs(step.charge-1000.0/360)<0.0001);
    CHECK("trapezoidal mWh",step.energyValid && std::fabs(step.energy-4000.0/360)<0.0001);
    b.values["current"]=-500;
    CHECK("changing current integration",std::fabs(integrate(a,b).charge-750.0/360)<0.0001);
    b.values.remove("current");
    CHECK("missing current is not zero",!integrate(a,b).chargeValid);
    b=a;b.utc+=10000;b.mono+=10000;b.values["current"]=100;
    CHECK("sign conflict does not integrate",!integrate(a,b).chargeValid);
    b=a;b.utc+=60000;b.mono+=60000;
    CHECK("gap does not integrate",integrate(a,b).breakReason=="sampling-gap");
    b=a;b.utc+=15000;b.mono+=10000;
    CHECK("clock adjustment does not integrate",integrate(a,b).breakReason=="clock-change");
    b=a;b.utc+=10000;b.mono+=10000;b.run="next";
    CHECK("restart does not integrate",integrate(a,b).breakReason=="collector-restart");
    b=a;b.utc+=10000;b.mono+=10000;b.batteryId=7;
    CHECK("battery replacement does not integrate",integrate(a,b).breakReason=="battery-changed");
    CHECK("charge is never a capacity test",!capacityEstimate("charge",1000,1,90,30,true,true).isValid());
    CHECK("short SOC range is insufficient",!capacityEstimate("discharge",100,1,90,80,true,true).isValid());
    CHECK("partial current coverage is insufficient",!capacityEstimate("discharge",1000,0.8,90,30,true,true).isValid());
    CHECK("live session is not a completed capacity test",!capacityEstimate("discharge",1000,1,90,30,false,true).isValid());
    CHECK("SOC reset rejects capacity estimate",!capacityEstimate("discharge",1000,1,90,30,true,false).isValid());
    CHECK("supported interval extrapolation",std::fabs(capacityEstimate("discharge",1200,1,90,30,true,true).toDouble()-2000)<0.0001);
    a.charger="NONE";a.values["current"]=-200;
    CHECK("unplugged discharge classification",modeFor(a)=="discharge");
    a.charger="PLUGGED";a.values["current"]=0;
    CHECK("plugged zero is idle, not 1470mA",modeFor(a)=="plugged");
    a.values["current"]=400;
    CHECK("positive battery current classification",modeFor(a)=="charge");
    a.values["current"]=-200;
    CHECK("external power does not imply charging",connected(a.charger)&&modeFor(a)=="discharge");
    QVariantMap row;row["utc_ms"]=100000;row["mono_ms"]=100000;row["run_id"]="r";row["battery_id"]=244;
    row["interval_s"]=10;row["ready"]=1;row["mode"]="discharge";row["session_id"]=1;
    row["current"]=-360;row["voltage"]=4000;row["temperature"]=25;
    HistorySummary summary(100000,120000);summary.add(row);
    row["utc_ms"]=110000;row["mono_ms"]=110000;row["current"]=-720;summary.add(row);
    QVariantMap totals=summary.result();
    CHECK("summary does not use first bucket sample",std::fabs(totals["currentMean"].toDouble()+540)<0.0001);
    CHECK("summary coverage uses requested duration",std::fabs(totals["coverage"].toDouble()-50)<0.001);
    CHECK("summary mAh uses trapezoids",std::fabs(totals["dischargeMah"].toDouble()-1.5)<0.0001);
    CHECK("summary preserves extrema",totals["currentMin"].toDouble()==-720&&totals["currentMax"].toDouble()==-360);
    HistorySummary clipped(102000,108000);row["utc_ms"]=100000;row["mono_ms"]=100000;row["current"]=-360;
    clipped.add(row);row["utc_ms"]=110000;row["mono_ms"]=110000;row["current"]=-720;clipped.add(row);
    CHECK("summary interpolates clipped boundaries",std::fabs(clipped.result()["dischargeMah"].toDouble()-0.9)<0.0001);
    row["gap_reason"]="paused";HistorySummary missing(100000,120000);missing.add(row);
    row["utc_ms"]=120000;row["mono_ms"]=120000;missing.add(row);
    CHECK("summary never integrates across gaps",missing.result()["coverage"].toDouble()==0&&!missing.result()["dischargeMah"].isValid());
    AlertConfig config;AlertMonitor alerts;a.ready=true;a.values["soc"]=20;a.values["temperature"]=45;a.charger="NONE";
    CHECK("threshold alerts trigger",alerts.evaluate(a,config).size()==2);
    CHECK("threshold alerts latch",alerts.evaluate(a,config).isEmpty());
    a.values["soc"]=23;a.values["temperature"]=44;
    CHECK("threshold hysteresis rejects chatter",alerts.evaluate(a,config).isEmpty());
    a.values["soc"]=26;a.values["temperature"]=41;alerts.evaluate(a,config);
    a.values["soc"]=19;a.values["temperature"]=45;
    CHECK("threshold alerts rearm after hysteresis",alerts.evaluate(a,config).size()==2);
    config.enabled=false;
    CHECK("alerts can be disabled",alerts.evaluate(a,config).isEmpty());
    QVariantMap source=sourceDetails("@status\nChargerInfo:json:{\"NumOfSource\":0}\n"
        "SystemInfo:json:{\"ChargeCurrent\":0}\n"
        "BatteryInfo:json:{\"ActiveBattery\":0,\"BatteryStatus\":[{\"TimeToEmpty\":1440,\"TimeToFull\":65535}]}\n");
    CHECK("structured PPS absent charger fallback",source["charger"]=="NONE");
    CHECK("system zero current remains valid",source["systemChargeCurrent"].isValid()&&source["systemChargeCurrent"].toInt()==0);
    CHECK("system time sentinel is unavailable",source["timeToEmpty"].toInt()==1440&&!source["timeToFull"].isValid());
    source=sourceDetails("@status\nChargerInfo:json:{\"NumOfSource\":1,\"ActiveCharger\":0,\"ChargerParams\":[{\"ChargerStatus\":\"CHARGING\"}]}\n");
    CHECK("structured PPS connected charger fallback",source["charger"]=="CHARGING");
#undef CHECK
    *report=passed.join("\n")+QString("\nRESULT: PASS (%1 checks)\n").arg(passed.size());return true;
}
}
