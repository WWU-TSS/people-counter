#include "mbed.h"
#include "lwip/opt.h"
#include "lwip/stats.h"
#include "lwip/sys.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"
#include "lwip/tcp.h"
#include "lwip/dns.h"
#include "lwip/dhcp.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/ip_addr.h"
#include "netif/etharp.h"
#include "netif/loopif.h"
#include "device.h"
#include <string>
#include <deque>
#include <ctime>
#include <sstream>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include "TextLCD.h"
#include "bigmath.h"

using namespace std;

string HostName = "192.168.1.1";
int HostPort = 8080;
const char * const TimeServer = "time.nist.gov";
const int TimePort = 37;
const char * const shortTimeFormat = "%I:%M%p %m/%d/%y";
#define TIME_ZONE_STRING "PST"
#define TIME_ZONE_STRING_DST "PDT"
const int TimeZoneHourOffset = -8;
enum DaylightSavingsTimeType
{
    Auto,
    On,
    Off
};
const DaylightSavingsTimeType DaylightSavingsTime = Auto;
static bool isDaylightSavingsTimeInEffect(time_t t);
const char * longTimeFormat(time_t t)
{
    if(isDaylightSavingsTimeInEffect(t))
        return "%I:%M:%S %p %m/%d/%y " TIME_ZONE_STRING_DST;
    return "%I:%M:%S %p %m/%d/%y " TIME_ZONE_STRING;
}
const int SupersampleFactor = 2;
const float LEDPeriod = 0.005;

TextLCD lcd(p13, p14, p17, p18, p19, p20);
LocalFileSystem lfs("local");

Ethernet ethernet;
struct netif netif_data;
struct netif *netif = &netif_data;
bool linkLED = false;
Timeout ledTimeout;

class Watchdog 
{
public:
// Load timeout value in watchdog timer and enable
    static void kick(float s) 
    {
        LPC_WDT->WDCLKSEL = 0x1;                // Set CLK src to PCLK
        uint32_t clk = SystemCoreClock / 16;    // WD has a fixed /4 prescaler, PCLK default is /4
        LPC_WDT->WDTC = s * (float)clk;
        LPC_WDT->WDMOD = 0x3;                   // Enabled and Reset
        kick();
    }
// "kick" or "feed" the dog - reset the watchdog timer
// by writing this required bit pattern
    static void kick() 
    {
        LPC_WDT->WDFEED = 0xAA;
        LPC_WDT->WDFEED = 0x55;
    }
};

volatile bool gotTime = false;
volatile bool gettingTime = false;

static bool isDaylightSavingsTimeInEffect(time_t t)
{
    const int factor = 3600; // seconds in an hour
    t += (size_t)((long)factor * TimeZoneHourOffset);
    switch(DaylightSavingsTime)
    {
    case Off:
        return false;
    case On:
        return true;
    default: // Auto
        break;
    }
    tm timeinfo = *localtime(&t);
    if(timeinfo.tm_mon < 2 || timeinfo.tm_mon > 10) // before march or after november
        return false;
    if(timeinfo.tm_mon > 2 && timeinfo.tm_mon < 10) // after march and before november
        return true;
    int previousSunday = timeinfo.tm_mday - timeinfo.tm_wday;
    if(timeinfo.tm_mon == 2) // if it's march
    {
        return previousSunday >= 8;
    }
    else // otherwise it's november
    {
        return previousSunday <= 0;
    }
}

static time_t getLocalTime(time_t utc)
{
    const int factor = 3600; // seconds in an hour
    time_t retval = utc;
    if(isDaylightSavingsTimeInEffect(retval))
        retval += (size_t)factor;
    retval += (size_t)((long)factor * TimeZoneHourOffset);
    return retval;
}

static err_t timeReceiveCallback(void *, tcp_pcb *tcp, pbuf *p, err_t err)
{
    tcp_recv(tcp, NULL);
    gettingTime = false;
    if(p)
    {
        if(p->tot_len >= 4 && !gotTime)
        {
            u32_t v = *(u32_t *)p->payload;
            gotTime = true;
            srand(v);
            time_t tv = ntohl(v) - 2208988800U;
            set_time(tv);
            tv = getLocalTime(tv);
            tm * timeinfo = localtime(&tv);
            char str[50];
            strftime(str, sizeof(str), longTimeFormat(time(NULL)), timeinfo);
            printf("Got Time : %u %s\r\n", tv, str);
        }
        else
        {
            tcp_abort(tcp);
            return ERR_ABRT;
        }
        tcp_recved(tcp, p->tot_len);
        pbuf_free(p);
        return tcp_close(tcp);
    }
    else if(err == ERR_OK)
        return tcp_close(tcp);
    tcp_abort(tcp);
    return ERR_ABRT;
}

static err_t timeConnectedCallback(void *, tcp_pcb * tcp, err_t)
{
    tcp_recv(tcp, timeReceiveCallback);
    return ERR_OK;
}

static void timeErrorCallback(void *, err_t)
{
    gettingTime = false;
}

static void gotTimeIP(ip_addr ip)
{
    tcp_pcb * tcp = tcp_new();
    tcp_err(tcp, timeErrorCallback);
    printf("Connecting to %s:%u...\r\n", inet_ntoa(*(in_addr *)&ip), (unsigned)TimePort);
    if(ERR_OK != tcp_connect(tcp, &ip, TimePort, timeConnectedCallback))
    {
        tcp_abort(tcp);
        gettingTime = false;
    }
}

static void gotTimeIPCallback(const char *, ip_addr * ip, void *)
{
    if(!ip || !ip->addr)
    {
        gettingTime = false;
        return;
    }
    gotTimeIP(*ip);
}

static void getTime()
{
    if(gettingTime || gotTime)
        return;
    gettingTime = true;
    in_addr addr;
    static ip_addr ip;
    if(inet_aton(TimeServer, &addr))
    {
        ip.addr = addr.s_addr;
        gotTimeIP(ip);
        return;
    }
    printf("Resolving %s...\r\n", TimeServer);
    switch(dns_gethostbyname(TimeServer, &ip, &gotTimeIPCallback, NULL))
    {
    case ERR_OK:
        gotTimeIP(ip);
        return;
    case ERR_INPROGRESS:
        return;
    default:
        gettingTime = false;
        return;
    }
}

class SendStringToHostHelper
{
    SendStringToHostHelper * next;
    SendStringToHostHelper * prev;
    static SendStringToHostHelper * head;
    string data, host;
    void (*callback)(bool successful, void *);
    void * callbackArg;
    ip_addr ip;
    int port;
    tcp_pcb * tcp;
    size_t amountSentAlready;
    int pollCount;
    volatile bool done;
    volatile bool resolving;
    void errorCallback()
    {
        if(callback && !done)
            callback(false, callbackArg);
        done = true;
    }
    void successCallback()
    {
        if(callback && !done)
            callback(true, callbackArg);
        done = true;
    }
    static void errorCallback(void * arg, err_t err)
    {
        //printf("error\r\n");
        fflush(stdout);
        if(!arg)
            return;
        SendStringToHostHelper * me = (SendStringToHostHelper *)arg;
        me->errorCallback();
    }
    void queueChunk()
    {
        size_t sendAmount = data.size() - amountSentAlready;
        size_t maxSendAmount = tcp_sndbuf(tcp);
        u8_t flags = TCP_WRITE_FLAG_COPY;
        if(maxSendAmount < sendAmount)
        {
            sendAmount = maxSendAmount;        
            flags |= TCP_WRITE_FLAG_MORE;
        }
        tcp_write(tcp, (void *)(data.c_str() + amountSentAlready), sendAmount, flags);        
        amountSentAlready += sendAmount;
        //printf("Sending %u bytes.\r\n", (unsigned)sendAmount);
    }
    void pollCallback()
    {
        //printf("polled\r\n");
        tcp_pcb * tcp = this->tcp;
        if(pollCount++ > 3)
        {
            tcp_arg(tcp, NULL);
            tcp_sent(tcp, NULL); 
            tcp_err(tcp, NULL);           
            tcp_abort(tcp);
            errorCallback();
            return;
        }
    }
    static err_t sentCallback(void *arg, struct tcp_pcb *tcp, u16_t len)
    {
        if(arg)
        {
            SendStringToHostHelper * me = (SendStringToHostHelper *)arg;
            if(me->amountSentAlready >= me->data.size())
            {
                me->successCallback();          
                tcp_err(tcp, NULL);
            }
            else
            {
                me->queueChunk();
                return tcp_output(tcp);
            }  
        }
        tcp_arg(tcp, NULL);
        tcp_sent(tcp, NULL); 
        return tcp_close(tcp);
    }
    static err_t connectedCallback(void * arg, tcp_pcb * tcp, err_t err)
    {       
        //printf("Connected.\r\n");
        SendStringToHostHelper * me = (SendStringToHostHelper *)arg;
        if(err == ERR_OK)
        {
            tcp_sent(tcp, &sentCallback);
            me->queueChunk();
        }
        else
        {
            tcp_arg(tcp, NULL);
            tcp_sent(tcp, NULL); 
            tcp_err(tcp, NULL);           
            errorCallback(arg, err);
            tcp_abort(tcp);
            return ERR_ABRT;
        }
        return err;
    }
    void onGotIP()
    {
        printf("Connecting to %s:%u...\r\n", inet_ntoa(*(struct in_addr*)&ip), (unsigned)port);
        tcp = tcp_new();
        if(!tcp)
        {
            errorCallback();
            return;
        }
        tcp_arg(tcp, (void *)this);
        tcp_err(tcp, &errorCallback);
        if(ERR_OK != tcp_connect(tcp, &ip, port, &connectedCallback))
        {
            tcp_arg(tcp, NULL);
            tcp_err(tcp, NULL);           
            tcp_abort(tcp);
            errorCallback();
            return;
        }
    }
    static void dnsResolveCallback(const char *name, ip_addr *ipaddr, void *arg)
    {
        SendStringToHostHelper * me = (SendStringToHostHelper *)arg;
        me->resolving = false;
        if(me->done)
            return;
        if(!ipaddr || !ipaddr->addr)
        {
            me->errorCallback();
            return;
        }
        me->ip = *ipaddr;
        me->onGotIP();
    }
    void abort()
    {
        tcp_arg(tcp, NULL);
        tcp_sent(tcp, NULL); 
        tcp_err(tcp, NULL);           
        tcp_abort(tcp);
        errorCallback();
    }
public:
    SendStringToHostHelper(string data, string host, int port, void (*callback)(bool successful, void *), void * callbackArg)
        : data(data), host(host), callback(callback), callbackArg(callbackArg), port(port), tcp(NULL), amountSentAlready(0), pollCount(0), done(false), resolving(false)
    {
        next = head;
        if(head)
            head->prev = this;
        prev = NULL;
        head = this;
    }
    ~SendStringToHostHelper()
    {
        if(prev)
            prev->next = next;
        else
            head = next;
        if(next)
            next->prev = prev;        
    }
    void start()
    {
        in_addr addr;
        if(inet_aton(host.c_str(), &addr))
        {
            ip.addr = addr.s_addr;
            onGotIP();
            return;
        }
        printf("Resolving %s...\r\n", host.c_str());
        resolving = true;
        switch(dns_gethostbyname(host.c_str(), &ip, &dnsResolveCallback, (void *)this))
        {
        case ERR_OK:
            resolving = false;
            onGotIP();
            return;
        case ERR_INPROGRESS:
            return;
        default:
            resolving = false;
            errorCallback();
            return;
        }
    }
    static void poll()
    {
        if(!head)
            return;
        SendStringToHostHelper * i = head;
        SendStringToHostHelper * next = i->next;
        for(; i != NULL; i = next, next = i->next)
        {
            if(i->done)
            {
                delete i;
                continue;
            }
            i->pollCallback();
        }
    }
    static void killall()
    {
        if(!head)
            return;
        SendStringToHostHelper * i = head;
        SendStringToHostHelper * next = i->next;
        for(; i != NULL; i = next, next = i->next)
        {
            if(i->done && !i->resolving)
            {
                delete i;
                continue;
            }
            i->abort();
        }
    }
};

SendStringToHostHelper * SendStringToHostHelper::head = NULL;
volatile bool canPoll = false;

void onPollTick()
{
    canPoll = true;
}

void sendStringToHost(string data, string host, int port, void (*callback)(bool successful, void *), void * callbackArg)
{
    (new SendStringToHostHelper(data, host, port, callback, callbackArg))->start();
}

volatile int connectionsActive = 0;
DigitalOut sending(LED3);

void myCallback(bool successful, void *)
{
    printf(successful ? "succeded\r\n" : "failed\r\n");
    fflush(stdout);
    if(--connectionsActive <= 0)
        sending = false;
}

volatile bool canSend = true;

void onSendTick()
{
    canSend = true;
}

void onIdle();

void restartLink()
{
    struct netif *netif = &netif_data;
    //printf("Disconnected.\r\n");
    netif_set_link_down(netif);
    dhcp_stop(netif);
    while(!ethernet.link())
    {
        linkLED = ethernet.link();
        device_poll();
        onIdle();
    }
    netif_set_link_up(netif);
    dhcp_start(netif);
    while (!netif_is_up(netif)) 
    { 
        linkLED = ethernet.link();
        device_poll();          
        onIdle();
    } 
    //printf("Interface is up, local IP is %s\r\n", inet_ntoa(*(struct in_addr*)&(netif->ip_addr))); 
}

const int EventLogSize = 20;
deque<string> EventLog;
volatile int sendingCount = 0;
volatile int sentCount = 0;

void addEvent(string event)
{
    ostringstream os;
    os << hex << time(NULL) << " " << event;
    if(EventLog.size() >= EventLogSize)
    {
        EventLog.pop_front();
    }
    EventLog.push_back(os.str());
}

void sendEventsCallback(bool successful, void *)
{
    printf(successful ? "synced log\r\n" : "can't connect to log server\r\n");
    fflush(stdout);
    if(successful && sendingCount > 0)
    {
        sentCount = sendingCount;
        sendingCount = 0;
    }
    if(--connectionsActive <= 0)
        sending = false;
}

string getStatsString()
{
    ostringstream os;
    os << hex << time(NULL) << "\n";
    return os.str();
}

void sendString(string data);

void sendEvents()
{
    string data = getStatsString();
    sendingCount = EventLog.size();
    deque<string>::iterator iter = EventLog.begin();
    for(int i = 0; i < sendingCount; i++)
    {
        data += *iter++;
        data += "\n";
    }
    sendString(data);
}

volatile bool canRunLEDSense = false;

void onLEDTick()
{
    canRunLEDSense = true;
}

struct SingleSensorChannel
{
    enum State
    {
        Blocked,
        Blocking,
        Unblocked,
        Unblocking
    };
    State state;
    AnalogIn sensor;
    DigitalOut display;
    bool onValueSet, offValueSet;
    float onValue, offValue;
    float nextOnValue, nextOffValue;
    int supersampleCount;
    float averageDelta;
    enum {startUpCycleCount = 20};
    int averagingCycles;
    SingleSensorChannel(PinName sensorPin, PinName displayPin)
        : state(Unblocked), sensor(sensorPin), display(displayPin), onValueSet(false), offValueSet(false), onValue(0), offValue(0), nextOnValue(0), nextOffValue(0), supersampleCount(0), averageDelta(0), averagingCycles(0)
    {
    }
    void staticifyState() // change state so that it's not changing
    {
        if(state == Blocking)
            state = Blocked;
        else if(state == Unblocking)
            state = Unblocked;
    }
    bool isBlocked() const
    {
        return state == Blocked || state == Blocking;
    }
    void setChangingState(bool blocked)
    {
        if(blocked)
        {
            if(!isBlocked())
                state = Blocking;
        }
        else
        {
            if(isBlocked())
                state = Unblocking;
        }
    }
    void run(bool isLEDOn)
    {
        supersampleCount++;
        if(supersampleCount >= SupersampleFactor * 2)
        {
            supersampleCount = 0;
        }
        bool isLastSample = (supersampleCount == 0 || supersampleCount == SupersampleFactor * 2 - 1);
        if(isLEDOn)
        {
            nextOnValue += sensor / SupersampleFactor;
            if(isLastSample)
            {
                onValue = nextOnValue;
                nextOnValue = 0;
                onValueSet = true;
            }
        }
        else
        {
            nextOffValue += sensor / SupersampleFactor;
            if(isLastSample)
            {
                offValue = nextOffValue;
                nextOffValue = 0;
                offValueSet = true;
            }
        }
        if(!isLastSample)
            return;
        if(!onValueSet || !offValueSet)
            return;
        float delta = onValue - offValue;
        if(delta < 0) 
            delta = 0;   
        const float speedThreshold = 0.01, lowerValueThreshold = 0.007, upperValueThreshold = 0.015;
#if 0                
        if(averagingCycles == 0)
            averageDelta = delta;
        else
            averageDelta += 0.3 * (delta - averageDelta);
        if(averagingCycles < startUpCycleCount)
        {
            averagingCycles++;
            return;
        }
        if(averageDelta < lowerValueThreshold)
            setChangingState(true);
        else if(averageDelta > upperValueThreshold)
            setChangingState(false);
#if 1            
        else if(delta - averageDelta > speedThreshold)
            setChangingState(false);
        else if(averageDelta - delta > speedThreshold)
            setChangingState(true);
#endif            
#else
        averageDelta = delta;
        if(delta < lowerValueThreshold)
            setChangingState(true);
        else if(delta > upperValueThreshold)
            setChangingState(false);
#endif   
        display = !isBlocked();
    }
};

DigitalOut sensorLEDPower(p23);
SingleSensorChannel outsideSensor(p15, LED4);
SingleSensorChannel insideSensor(p16, LED1);

void addEvent(string msg);

void onGoInside()
{
    printf("went inside\r\n");
    fflush(stdout);
    addEvent("in");
}

void onGoOutside()
{
    printf("went outside\r\n");
    fflush(stdout);
    addEvent("out");
}

enum SensorStateType
{
    Nothing,
    GoingInFirstBlocked,
    GoingInBothBlocked,
    GoingInLastBlocked,
    GoingOutFirstBlocked,
    GoingOutBothBlocked,
    GoingOutLastBlocked
};

template <typename T>
string toString(const T & v)
{
    ostringstream os;
    os << v;
    return os.str();
}

string fixedWidthFloatToString(float v, char positiveSign = ' ', char integerPadding = '0', char decimalPadding = '0', size_t integerPlaces = 1, size_t decimalPlaces = 3)
{
    char sign = positiveSign;
    if(v < 0)
    {
        sign = '-';
        v = -v;
    }
    int intPart = (int)floor(v);
    v -= floor(v);
    string decimalPartString = toString(v);
    size_t decimalPos = decimalPartString.find_first_of('.');
    if(decimalPos == string::npos)
        decimalPartString = "";
    else
        decimalPartString.erase(0, decimalPos + 1);
    if(decimalPartString.size() > decimalPlaces)
    {
        char lastDigit = decimalPartString[decimalPlaces];
        if(lastDigit >= '5') // need to round
        {
            bool carry = true;
            for(size_t i = 0, j = decimalPlaces - 1; i < decimalPlaces; i++, j--)
            {
                if(decimalPartString[j] == '9')
                {
                    decimalPartString[j] = decimalPadding;
                }
                else
                {
                    decimalPartString[j]++;
                    carry = false;
                    break;
                }
            }
            if(carry)
            {
                intPart++;
            }
        }
    }
    decimalPartString.resize(decimalPlaces, decimalPadding);
    string intPartString = toString(intPart);
    if(intPartString.size() < integerPlaces)
        intPartString = string(integerPlaces - intPartString.size(), integerPadding) + intPartString;
    return sign + intPartString + "." + decimalPartString;
}

void runLEDSense()
{
    if(!canRunLEDSense)
        return;
    canRunLEDSense = false;
    bool isLEDOn = sensorLEDPower;
    outsideSensor.run(isLEDOn);
    insideSensor.run(isLEDOn);
    sensorLEDPower = !isLEDOn;
    ledTimeout.attach(&onLEDTick, LEDPeriod / SupersampleFactor);
    
    static SensorStateType state = Nothing;
    
    switch(state)
    {
    case GoingInFirstBlocked:
        if(outsideSensor.isBlocked())
            state = GoingInBothBlocked;
        else if(!insideSensor.isBlocked())
            state = Nothing;
        break;
    case GoingInBothBlocked:
        if(!insideSensor.isBlocked())
            state = GoingInLastBlocked;
        else if(!outsideSensor.isBlocked())
            state = GoingInFirstBlocked;
        break;
    case GoingInLastBlocked:
        if(insideSensor.isBlocked())
            state = GoingInBothBlocked;
        else if(!outsideSensor.isBlocked())
        {
            state = Nothing;
            onGoInside();
        }
        break;
    case GoingOutFirstBlocked:
        if(insideSensor.isBlocked())
            state = GoingOutBothBlocked;
        else if(!outsideSensor.isBlocked())
            state = Nothing;
        break;
    case GoingOutBothBlocked:
        if(!outsideSensor.isBlocked())
            state = GoingOutLastBlocked;
        else if(!insideSensor.isBlocked())
            state = GoingOutFirstBlocked;
        break;
    case GoingOutLastBlocked:
        if(outsideSensor.isBlocked())
            state = GoingOutBothBlocked;
        else if(!insideSensor.isBlocked())
        {
            state = Nothing;
            onGoOutside();
        }
        break;
    default:
        state = Nothing;
        if(outsideSensor.isBlocked())
            state = GoingOutFirstBlocked;
        else if(insideSensor.isBlocked())
            state = GoingInFirstBlocked;        
        break;
    }
    
    outsideSensor.staticifyState();
    insideSensor.staticifyState();
    lcd.locate(0, 1);
    string str = (insideSensor.isBlocked() ? "B " : "U ") + fixedWidthFloatToString(insideSensor.onValue - insideSensor.offValue);
    str.resize(7, ' ');
    str += (outsideSensor.isBlocked() ? " B " : " U ") + fixedWidthFloatToString(outsideSensor.onValue - outsideSensor.offValue);    
    str.resize(16, ' ');
    lcd.printf("%s", str.c_str());
}

DigitalOut ipUp(LED2);

enum StartupState
{
    StartingEthernet,
    EthernetDown,
    StartingDHCP,
    Running
};

StartupState startupState = StartingEthernet;

enum DisplayInfoState
{
    DisplayTime,
    DisplayIPAddress,
    DisplayLast
};
volatile DisplayInfoState displayInfoState = DisplayTime;

void handleDisplayInfoTick()
{
    displayInfoState = (DisplayInfoState)(((int)displayInfoState + 1) % (int)DisplayLast);
}

void onIdle()
{
    Watchdog::kick();
    if(canPoll)
    {
        canPoll = false;
        SendStringToHostHelper::poll();
    }
    ipUp = netif_is_up(&netif_data) && netif_is_link_up(&netif_data);
    if(sentCount > 0)
    {
        while(sentCount-- > 0)
        {
            EventLog.pop_front();
        }
    }
    if(ipUp)
        getTime();
    string msg;
    if(!gotTime || startupState != Running)
    {
        switch(startupState)
        {
        case StartingEthernet:
            msg = "Init Ethernet...";
            break;
        case EthernetDown:
            msg = "Ethernet Down.";
            break;
        case StartingDHCP:
            msg = "Init DHCP...";
            break;
        case Running:
            msg = "Init Time...";
            break;
        }
    }
    else
    {
        switch(displayInfoState)
        {
        case DisplayTime:
        {
            char str[50];
            time_t t = getLocalTime(time(NULL));
            strftime(str, sizeof(str), shortTimeFormat, localtime(&t));
            msg = str;
            break;
        }
        case DisplayIPAddress:
            msg = inet_ntoa(*(struct in_addr*)&(netif->ip_addr));
            break;
        }
    }
    msg.resize(16, ' ');
    lcd.locate(0, 0);
    lcd.printf("%s", msg.c_str());
    if(gotTime)
    {
        runLEDSense();
        if((canSend || EventLog.size() > EventLogSize / 2) && !sending && startupState == Running)
        {
            canSend = false;
            sendEvents();
        }
    }
}

void startInternet()
{
    netif = &netif_data;
    struct ip_addr ipaddr;
    struct ip_addr netmask;
    struct ip_addr gateway;
    /* Start Network with DHCP */
    IP4_ADDR(&netmask, 255,255,255,255);
    IP4_ADDR(&gateway, 0,0,0,0);
    IP4_ADDR(&ipaddr, 0,0,0,0);

    netif->hwaddr_len = ETHARP_HWADDR_LEN;
    device_address((char *)netif->hwaddr);
    netif = netif_add(netif, &ipaddr, &netmask, &gateway, NULL, device_init, ip_input);
    assert(netif);
    netif->hostname = "people-counter1";
    netif_set_default(netif);
    dhcp_start(netif); 
    while (!netif_is_up(netif)) 
    { 
        linkLED = ethernet.link();
        if(!ethernet.link())
            startupState = EthernetDown;
        else
            startupState = StartingDHCP;
        device_poll();          
        onIdle();
    } 
    startupState = Running;
}

void stopInternet()
{
    dhcp_stop(netif);
    netif_set_down(netif);
    netif_remove(netif);
    netif = &netif_data;
    startupState = EthernetDown;
}

BigUnsigned encryptionModulus = (WordType)0;
BigUnsigned encryptionExponent = (WordType)0x10001;
string deviceName = "people-counter";

void loadSettings()
{
    {
        ifstream is("/local/host.txt");
        if(is)
        {
            getline(is, HostName);
            is >> HostPort;
            is.close(); 
        }
    }
    {
        ifstream is("/local/enc-key.txt");
        if(is)
        {
            string key;
            is >> key;
            is.close();
            encryptionModulus = BigUnsigned::parseHexByteString(key);
            if(encryptionModulus == (WordType)0)
            {
                printf("invalid encryption modulus\r\n");
                fflush(stdout);
                while(true)
                    ;
            }
        }        
    }
    {
        ifstream is("/local/name.txt");
        if(is)
        {
            is >> deviceName;
        }
    }
}

WordType randomEngine()
{
    WordType v = rand() & 0xFF;
    for(size_t i = 1; i < BytesPerWord; i++)
    {
        v <<= 8;
        v |= rand() & 0xFF;
    }
    return v;
}

BigUnsigned randomBits(size_t bitCount)
{
    BigUnsigned retval = randomEngine() & (((WordType)1 << (bitCount % BitsPerWord)) - 1);
    for(size_t i = BitsPerWord; i < bitCount; i += BitsPerWord)
    {
        retval <<= BitsPerWord;
        retval += randomEngine();
    }    
    return retval;
}

string encryptString(string textIn)
{
    if(encryptionModulus == (WordType)0)
        return "0" + textIn;
    string retval = "1";
    const size_t encryptChunkSize = 32;
    printf("encryptString\r\ntextIn : %s\r\n", textIn.c_str());
    for(size_t i = 0; i < textIn.size(); i += encryptChunkSize)
    {
        const size_t randomBitCount = 64;
        const WordType checkSumModulus = 8191;
        BigUnsigned v = BigUnsigned::fromByteString(textIn.substr(i, encryptChunkSize));
        v = (v << randomBitCount) + randomBits(randomBitCount);
        WordType checkSum = (WordType)(v % checkSumModulus);
        v *= checkSumModulus;
        v += checkSum;
        v = powMod(v, encryptionExponent, encryptionModulus);
        retval += v.toBase64() + "\n"; 
    }
    return retval;
}

void sendString(string data)
{
    data = deviceName + "\n" + data;
    data = encryptString(data);
    connectionsActive++;
    sending = true;
    sendStringToHost(data, HostName, HostPort, &sendEventsCallback, NULL);
}

int main() 
{
    loadSettings();
    Watchdog::kick(3);
    printf("\x1b[2J\x1b[H");
    fflush(stdout);
    lcd.cls();
    Ticker tickFast, tickSlow, tickARP, eth_tick, dns_tick, dhcp_coarse, dhcp_fine;
    Ticker pollTick, displayInfoTick;
    pollTick.attach(&onPollTick, 1);
    ledTimeout.attach(&onLEDTick, LEDPeriod / SupersampleFactor);
    displayInfoTick.attach(&handleDisplayInfoTick, 5);

    /* Initialise after configuration */
    lwip_init();
    
        /* Initialise all needed timers */
    tickARP.attach_us( &etharp_tmr,  ARP_TMR_INTERVAL  * 1000);
    tickFast.attach_us(&tcp_fasttmr, TCP_FAST_INTERVAL * 1000);
    tickSlow.attach_us(&tcp_slowtmr, TCP_SLOW_INTERVAL * 1000);
    dns_tick.attach_us(&dns_tmr, DNS_TMR_INTERVAL * 1000);
    dhcp_coarse.attach_us(&dhcp_coarse_tmr, DHCP_COARSE_TIMER_MSECS * 1000);
    dhcp_fine.attach_us(&dhcp_fine_tmr, DHCP_FINE_TIMER_MSECS * 1000);
    startInternet();

    //printf("Interface is up, local IP is %s\r\n", inet_ntoa(*(struct in_addr*)&(netif->ip_addr))); 
    Ticker tickSend;
    tickSend.attach(&onSendTick, 10);
    canSend = false;
    while(1) 
    {
        device_poll();
        linkLED = ethernet.link();
        if(!ethernet.link())
        {
            stopInternet();
            startInternet();
        }
        onIdle();
    }
}
