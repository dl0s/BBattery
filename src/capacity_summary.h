#ifndef BBATTERY_CAPACITY_SUMMARY_H
#define BBATTERY_CAPACITY_SUMMARY_H
#include <string>

namespace Battery { namespace Capacity {
inline bool systemReference(bool sameBattery,bool attributed,bool ready,bool knownSource,
                            bool recordIdentified,bool timeIdentified,double value){
    return sameBattery&&attributed&&ready&&knownSource&&recordIdentified&&timeIdentified&&value>=1&&value<=20000;
}
// Only timed tests can supply a test estimate. Never scan legacy sessions/samples.
// Revalidate persisted results so malformed/partial older rows cannot win selection.
inline std::string summarySql(){
    return "SELECT * FROM capacity_tests WHERE battery_key=? AND mode=? "
        "AND status='completed' AND reason IN ('timer','manual') "
        "AND end_ms IS NOT NULL AND end_ms>=start_ms AND samples>=2 AND stable_soc=1 "
        "AND elapsed_s>0 AND integrated_s>=0.95*elapsed_s AND integrated_s<=1.001*elapsed_s "
        "AND start_soc BETWEEN 0 AND 100 AND end_soc BETWEEN 0 AND 100 "
        "AND capacity>=1 AND capacity<=20000 "
        "AND ((mode='discharge' AND start_soc-end_soc>=30 AND discharge_mah>0 AND discharge_mah<=20000 "
        "AND ABS(capacity-discharge_mah*100.0/(start_soc-end_soc))<=0.01) "
        "OR (mode='charge' AND end_soc-start_soc>=30 AND charge_mah>0 AND charge_mah<=20000 "
        "AND ABS(capacity-charge_mah*100.0/(end_soc-start_soc))<=0.01)) "
        "ORDER BY end_ms DESC,id DESC LIMIT 1";
}
inline std::string summaryIndex(){
    // Optional optimization for SQLite >=3.8.0. BB10's 3.7.14 uses the ordinary index.
    return "CREATE INDEX IF NOT EXISTS capacity_tests_estimate ON capacity_tests(battery_key,mode,end_ms DESC,id DESC) "
        "WHERE status='completed' AND capacity>=1 AND capacity<=20000";
}
} }
#endif
