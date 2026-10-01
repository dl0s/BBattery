#include "../src/capacity.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

int main(int argc,char **argv){
    if(argc==2&&std::strcmp(argv[1],"--sql")==0){std::puts(Battery::Capacity::historySql());return 0;}
    int failed=0,total=0;double value=-1;
#define CHECK(name,condition) do { ++total; if(!(condition)){++failed;std::printf("FAIL: %s\n",name);} } while(0)
    using namespace Battery::Capacity;
    CHECK("discharge interval",interval(false,1200,1,90,30,true,true,value)&&std::fabs(value-2000)<0.0001);
    CHECK("charge interval",interval(true,1200,1,20,80,true,true,value)&&std::fabs(value-2000)<0.0001);
    CHECK("charge direction",!interval(true,1200,1,80,20,true,true,value));
    CHECK("discharge direction",!interval(false,1200,1,20,80,true,true,value));
    CHECK("short span",!interval(false,100,1,80,51,true,true,value));
    CHECK("coverage threshold",interval(false,600,.95,80,50,true,true,value));
    CHECK("missing coverage",!interval(false,600,.949,80,50,true,true,value));
    CHECK("impossible coverage",!interval(false,600,2,80,50,true,true,value));
    CHECK("live interval",!interval(false,1200,1,90,30,false,true,value));
    CHECK("SOC reversal",!interval(true,1200,1,20,80,true,false,value));
    CHECK("invalid endpoints",!interval(false,1200,1,120,30,true,true,value));
    CHECK("zero integral",!interval(false,0,1,90,30,true,true,value));
    CHECK("NaN integral",!interval(false,std::numeric_limits<double>::quiet_NaN(),1,90,30,true,true,value));
    CHECK("infinite integral",!interval(false,std::numeric_limits<double>::infinity(),1,90,30,true,true,value));
    CHECK("NaN coverage",!interval(false,1200,std::numeric_limits<double>::quiet_NaN(),90,30,true,true,value));
    CHECK("remaining and SOC",remaining(1000,50,value)&&std::fabs(value-2000)<0.0001);
    CHECK("remaining threshold",remaining(400,20,value)&&std::fabs(value-2000)<0.0001);
    CHECK("low SOC",!remaining(100,19,value));
    CHECK("zero SOC",!remaining(100,0,value));
    CHECK("zero remaining",!remaining(0,50,value));
    CHECK("sentinel remaining",!remaining(80000000,50,value));
    CHECK("oversized extrapolation",!remaining(20000,20,value));
    CHECK("health calculation",health(2100,90,value)&&std::fabs(value-1890)<0.0001);
    CHECK("zero health",!health(2100,0,value));
    CHECK("missing design",!health(-1,90,value));
    CHECK("invalid health",!health(2100,101,value));
    CHECK("charge SOC jitter",followsSoc(true,40,38));
    CHECK("charge SOC reversal",!followsSoc(true,40,37));
    CHECK("discharge SOC jitter",followsSoc(false,40,42));
    CHECK("discharge SOC reversal",!followsSoc(false,40,43));
    std::printf("Capacity calculation tests: %d/%d PASS\n",total-failed,total);
    return failed?1:0;
}
