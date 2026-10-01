#ifndef BBATTERY_CAPACITY_H
#define BBATTERY_CAPACITY_H

namespace Battery { namespace Capacity {
inline bool interval(bool charging,double mah,double coverage,int start,int end,
                     bool closed,bool stable,double &result){
    const int span=charging?end-start:start-end;
    if(!closed||!stable||start<0||start>100||end<0||end>100||span<30||
       !(mah>0&&mah<=20000)||!(coverage>=0.95&&coverage<=1.001))return false;
    const double value=mah*100.0/span;
    if(!(value>=1&&value<=20000))return false;
    result=value;return true;
}
inline bool remaining(double mah,double soc,double &result){
    if(!(mah>0&&mah<=20000)||!(soc>=20&&soc<=100))return false;
    const double value=mah*100.0/soc;
    if(!(value>=1&&value<=20000))return false;
    result=value;return true;
}
inline bool health(double design,double percent,double &result){
    if(!(design>=1&&design<=20000)||!(percent>0&&percent<=100))return false;
    const double value=design*percent/100.0;
    if(!(value>=1&&value<=20000))return false;
    result=value;return true;
}
inline bool followsSoc(bool charging,int previous,int next){
    return charging?next>=previous-2:next<=previous+2;
}
// Read legacy samples as well: older collectors did not flag charging SOC reversals.
inline const char *historySql(){
    return "SELECT s.* FROM sessions s WHERE s.mode=? AND s.battery_id=? "
        "AND (?=0 OR s.id=?) AND s.end_ms IS NOT NULL AND s.elapsed_s>0 "
        "AND s.covered_s>=0.95*s.elapsed_s AND s.covered_s<=1.001*s.elapsed_s "
        "AND s.stable_soc=1 AND s.mah>0 AND s.mah<=20000 "
        "AND s.start_soc BETWEEN 0 AND 100 AND s.end_soc BETWEEN 0 AND 100 "
        "AND ((s.mode='discharge' AND s.start_soc-s.end_soc>=30) "
        "OR (s.mode='charge' AND s.end_soc-s.start_soc>=30)) "
        "AND (?<>0 OR s.id>COALESCE((SELECT MAX(id) FROM sessions WHERE reason='battery-changed'),0)) "
        "AND NOT EXISTS (SELECT 1 FROM samples b WHERE b.session_id=s.id AND b.soc IS NOT NULL "
        "AND ((s.mode='charge' AND b.soc+2<(SELECT a.soc FROM samples a "
        "WHERE a.session_id=s.id AND a.soc IS NOT NULL AND a.id<b.id ORDER BY a.id DESC LIMIT 1)) "
        "OR (s.mode='discharge' AND b.soc-2>(SELECT a.soc FROM samples a "
        "WHERE a.session_id=s.id AND a.soc IS NOT NULL AND a.id<b.id ORDER BY a.id DESC LIMIT 1)))) "
        "ORDER BY s.id DESC LIMIT 1";
}
} }
#endif
