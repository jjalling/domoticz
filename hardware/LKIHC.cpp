/*!
 * \file hardware/LKIHC.cpp
 * \author  Jonas Bo Jalling <jonas@jalling.dk>
 * \version 0.1
 *
 * \section LICENSE
 *
 * The MIT License
 *
 * Copyright (c) 2017 Jonas Bo Jalling
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * \section DESCRIPTION
 *
 * This provides a interface to the LK IHC home automation system
 *
 */

/*
 * if (firstRun)
 * {
 *     connect to controller and get project info
 *     if (project info has changed)
 *     {
 *         update database with new items
 *     }
 * }
 *
 * if (watched devices > 0)
 * {
 *     connect to controller
 *     updated watchlist
 *
 *     wait for changes
 *     if changes
 *     {
 *         handle changes
 *     }
 *
 * }
 *
 */

#include "stdafx.h"
#include "LKIHC.h"
#include "../main/Helper.h"
#include "../main/Logger.h"
#include "../main/localtime_r.h"
#include "../main/SQLHelper.h"
#include "../main/mainworker.h"
#include "../main/WebServer.h"
#include "../webserver/cWebem.h"
#include "../httpclient/HTTPClient.h"
#include "hardwaretypes.h"
#include "../tinyxpath/tinyxml.h"
#include "../tinyxpath/xpath_static.h"
#include <boost/shared_ptr.hpp>
#include "IHC/IhcClient.hpp"
#include <cstring>
#include <exception>
#include <cstdlib>
#include <boost/lexical_cast.hpp>
#include <boost/format.hpp>
#include <map>

const int notification_timeout_s 5;
const int keep_alive = 30;

std::vector<int> activeResourceIdList;
CLKIHC::CLKIHC(const int ID, const std::string &IPAddress, const unsigned short Port, const std::string &Username, const std::string &Password) :
    m_IPAddress(IPAddress),
    m_UserName(Username),
    m_Password(Password)
{
    m_HwdID=ID;
    m_Port = Port;
    m_stoprequested=false;
    Init();
}

CLKIHC::~CLKIHC() = default;

ihcClient* ihcC;

void CLKIHC::Init()
{
}

bool CLKIHC::StartHardware()
{
    /*Start worker thread*/
    m_bIsStarted=true;
    sOnConnected(this);
    ihcC = new ihcClient(m_IPAddress, m_UserName, m_Password);

    m_thread = boost::shared_ptr<boost::thread>(new boost::thread(boost::bind(&CLKIHC::Do_Work, this)));

    return (m_thread!=NULL);
}

bool CLKIHC::StopHardware()
{
    if (m_thread!=NULL)
    {
        assert(m_thread);
        m_stoprequested = true;
        m_thread->join();
    }
    m_bIsStarted=false;
    return true;
}

using IhcDeviceID = uint64_t;
using IhcDeviceSerial = uint64_t;
using IhcDeviceSerialToID = std::multimap<IhcDeviceSerial, IhcDeviceID>;

struct IhcObject {
    IhcDeviceSerial SerialNumber{0};
    uint8_t Type{0};
    uint8_t SubType{0};
    uint8_t RSSI{0};
    uint8_t Battery{0};
    IhcObject () = default;
    IhcObject (IhcDeviceSerial serial, uint8_t ttype, uint8_t subtype, int rssi, int battery) :
        SerialNumber(serial), Type(ttype), SubType(subtype), RSSI(rssi), Battery(battery) {}
};

using IhcDeviceTypeCache = std::map<IhcDeviceID, IhcObject>;

IhcDeviceSerialToID IhcDeviceSerialToDeviceID;
IhcDeviceTypeCache DeviceCache;

void CLKIHC::Do_Work()
{
    _log.Log(LOG_STATUS,"LK IHC: Worker started...");
    auto crashCounter = 0;
    auto rssiAndBatteryUpdate = 0;
    auto firstTime = true;
    auto sec_counter = (keep_alive - 2);

    while (!m_stoprequested)
    {

        sec_counter++;
        m_LastHeartbeat = mytime(nullptr);

        if (ihcC->CONNECTED != ihcC->connectionState)
        {
#ifdef _DEBUG
            _log.Log(LOG_STATUS,"LK IHC: Connecting to IHC controller...");
#endif
            try
            {
                if (0 == sec_counter % keep_alive)
                {
                    ihcC->openConnection();
                }
            }
            catch (const char* msg)
            {
                _log.Log(LOG_ERROR, "LK IHC: Error: '%s'", msg);
                ihcC->reset();
                firstTime = true;
            }
        }
        else
        {

            try
            {
                if (firstTime)
                {
                    firstTime = false;
                    rssiAndBatteryUpdate = 0;
                    activeResourceIdList.clear();
                    DeviceCache.clear();
                    IhcDeviceSerialToDeviceID.clear();

                    // Lets create a device cache and a map for device serialnumbers -> deviceID
                    const auto result = m_sql.safe_query("SELECT DeviceID, Type, SubType, BatteryLevel, SignalLevel, Options FROM DeviceStatus WHERE HardwareID==%d", m_HwdID);
                    if (!result.empty())
                    {
                        for (auto itt = result.begin(); itt != result.end(); ++itt)
                        {
                            auto sd = *itt;

                            auto DeviceID = std::strtoul(sd[0].c_str(), NULL, 16);
                            auto DeviceType = atoi(sd[1].c_str());
                            auto DeviceSubType = atoi(sd[2].c_str());
                            auto DeviceSerialNumber = std::strtoul(sd[5].c_str(), NULL, 10);
                            auto DeviceBatteryValue = atoi(sd[3].c_str());
                            auto DeviceRSSIValue = atoi(sd[4].c_str());

                            IhcObject IhcDevice (DeviceSerialNumber, DeviceType, DeviceSubType, DeviceBatteryValue, DeviceRSSIValue);
                            DeviceCache.insert(std::make_pair( DeviceID, IhcDevice));

                            if (DeviceSerialNumber != 0)
                            {
                                IhcDeviceSerialToDeviceID.insert(std::make_pair(DeviceSerialNumber, DeviceID));
                            }
                        }
                    }

                    /*std::cout << "Cache length: " << DeviceCache.size() << std::endl;
                      std::cout << "Length: " << IhcDeviceSerialToDeviceID.size() << std::endl;*/
                    /*for(std::multimap<IhcDeviceSerial, IhcDeviceID>::iterator iter = IhcSerialList.begin(); iter != IhcSerialList.end(); ++iter)
                      {
                      IhcDeviceSerial k =  (*iter).first;
                      IhcDeviceID l = (*iter).second;
                    //std::cout << k << " .. " << l << std::endl;
                    //std::cout << cache[k].Type << "." << cache[k].SubType << std::endl;
                    //ignore value
                    //Value v = iter->second;
                    }
                    //WSProjectInfo inf = ihcC->getProjectInfo();
                    //_log.Log(LOG_STATUS, "LK IHC: Project info. %s", inf);
                    for(std::multimap<IhcDeviceSerial, IhcDeviceID>::iterator it = IhcSerialList.begin(), end = IhcSerialList.end(); it != end; it = IhcSerialList.upper_bound(it->first))
                    {
                    std::cout << it->first << ' ' << it->second << std::endl;

                    }
                    IhcDeviceSerial id = 109955793651093;
                    auto receiversEntries = IhcSerialList.equal_range(id);
                    for (auto receiverEntry = receiversEntries.first; receiverEntry != receiversEntries.second; ++receiverEntry)
                    {

                    std::cout << receiverEntry->second << std::endl;
                    }

*/
                }


                const auto result = m_sql.safe_query("SELECT DeviceID FROM DeviceStatus WHERE HardwareID==%d AND Used == 1", m_HwdID);
                if (!result.empty())
                {
                    if (ihcC->CONNECTED != ihcC->connectionState)
                    {
                        ihcC->openConnection();
                    }

                    if (rssiAndBatteryUpdate % 100 == 0)
                    {
                        UpdateBatteryAndRSSI();
                    }
                    rssiAndBatteryUpdate++;


                    std::vector<int> resourceIdList;
                    for (auto itt = result.begin(); itt != result.end(); ++itt)
                    {
                        std::vector<std::string> sd = *itt;
                        IhcDeviceID DeviceID = std::strtoul(sd[0].c_str(), NULL, 16);
                        resourceIdList.push_back(std::strtoul(sd[0].c_str(), NULL, 16));
                    }

                    if (resourceIdList != activeResourceIdList)
                    {
                        activeResourceIdList = resourceIdList;
                        ihcC->enableRuntimeValueNotification(activeResourceIdList);
                        _log.Log(LOG_STATUS, "LK IHC: Updating listener resource list with %d elements", activeResourceIdList.size());
                    }

                    //std::vector<boost::shared_ptr<ResourceValue> > updatedResources;
                    const auto updatedResources = ihcC->waitResourceValueNotifications(notification_timeout_s);

                    // Handle object state changes
                    //for (std::vector<boost::shared_ptr<ResourceValue> >::iterator it = updatedResources.begin(); it != updatedResources.end(); ++it)
                    for (auto  it = updatedResources.begin(); it != updatedResources.end(); ++it)
                    {
                        ResourceValue & obj = *(*it);

                        char szID[10];
                        std::sprintf(szID, "%08lX", (long unsigned int)obj.ID);
                        std::cout << (long unsigned int)obj.ID << std::endl;
                        std::cout << obj.ID << std::endl;

                        auto device = DeviceCache.find(obj.ID);
                        if (device != DeviceCache.end())
                        {
                            std::cout << "found something" << std::endl;
                            std::cout << int ((*device).second.Type) << std::endl;
                            if ((*device).second.Type ==  pTypeIHCDevice)
                            {
                                //MainWorker::decode_Temp();
                                std::cout << "got temp" << std::endl;
                                tIHCDevice lcmd;
                                lcmd.type = pTypeIHCDevice;



                                lcmd.id = (long unsigned int)obj.ID;
                                lcmd.subtype = 6;//(*device).second.SubType;

                                m_mainworker.PushAndWaitRxMessage(this, (const unsigned char *)&lcmd, NULL, (*device).second.Battery);
                            }
                            else
                            {
                                _tGeneralSwitch ycmd;
                                ycmd.subtype = (*device).second.SubType;
                                ycmd.rssi = (*device).second.RSSI;
                                ycmd.id =  (long unsigned int)obj.ID;
                                ycmd.unitcode = 0;
                                if (obj.intValue() > 1)
                                {
                                    ycmd.cmnd = 2;
                                    ycmd.level=obj.intValue();
                                }
                                else
                                {
                                    ycmd.cmnd = obj.intValue();
                                    ycmd.level=0;
                                }
                                m_mainworker.PushAndWaitRxMessage(this, (const unsigned char *)&ycmd, NULL, (*device).second.Battery);
                            }

                        }
                    }
                }
                else
                {
                    sleep_seconds(10);
                    ihcC->reset();
                }
            }
            catch (...)
            {
                crashCounter++;
                _log.Log(LOG_ERROR, "LK IHC: The IHC controller has crashed %d times since Domoticz was started", crashCounter);
                ihcC->reset();
                firstTime = true;
            }
        }
        sleep_seconds(1);
    }

    _log.Log(LOG_STATUS,"LK IHC: Worker stopped...");
}

bool CLKIHC::WriteToHardware(const char *pdata, const unsigned char length)
{
    auto result = false;
    if (ihcC->CONNECTED == ihcC->connectionState)
    {
        const auto *pSen = reinterpret_cast<const tRBUF*>(pdata);

        try
        {
            if (pSen->ICMND.packettype == pTypeGeneralSwitch)
            {
                const auto *general = reinterpret_cast<const _tGeneralSwitch*>(pdata);
                switch (pSen->ICMND.subtype)
                {

                    case sSwitchIHCFBInput:
                        {
                            /* Boolean value */
                            ResourceValue const output_value(general->id, general->cmnd == gswitch_sOn ? true : false);
                            result = ihcC->resourceUpdate(output_value);
                            break;
                        }
                    case sSwitchIHCFBOutput:
                        return false;
                        break;

                    case sSwitchIHCOutput:
                    case sSwitchIHCInput:
                        {
                            /* Boolean value */
                            ResourceValue const output_value(general->id, general->cmnd == gswitch_sOn ? true : false);
                            result = ihcC->resourceUpdate(output_value);
                            break;
                        }

                    case sSwitchIHCDimmer:
                        {
                            /* Integer value */
                            auto dimmer_value = 0;
                            if (general->cmnd == gswitch_sOff)
                            {
                                dimmer_value = 0;
                            }
                            else if (general->cmnd == gswitch_sOn)
                            {
                                dimmer_value = 100;
                            }
                            else
                            {
                                dimmer_value = general->level;
                            }
                            ResourceValue const output_value(general->id, RangedInteger(dimmer_value));
                            result = ihcC->resourceUpdate(output_value);
                            break;
                        }
                }
            }

            if (result)
            {
                _log.Log(LOG_STATUS, "LK IHC: Resource update was successful");
            }
            else
            {
                _log.Log(LOG_STATUS, "LK IHC: Failed resource update");
            }
        }
        catch (const char* msg)
        {
            _log.Log(LOG_ERROR, "LK IHC: Error: '%s'", msg);
            ihcC->reset();
        }

    }
    return result;
}

void CLKIHC::addDeviceIfNotExists(const TiXmlNode* device, const unsigned char deviceType, bool isFunctionBlock)
{
    IhcDeviceSerial serialNumber = 0;
    const auto deviceID = std::string(device->ToElement()->Attribute("id")).substr(3);

    char sid[10];
    sprintf(sid, "%08X", (std::stoul(deviceID, NULL, 16)));

    const auto result = m_sql.safe_query("SELECT ID FROM DeviceStatus WHERE (HardwareID==%d) AND (DeviceID=='%q')",
            m_HwdID, sid);
    if (result.size() < 1)
    {
#ifdef _DEBUG
        _log.Log(LOG_NORM, "LK IHC: Added device %d %s", m_HwdID, sid);
#endif
        char buff[100];

        if (isFunctionBlock)
        {
            snprintf(buff, sizeof(buff), "FB | %s | %s | %s",
                    device->Parent()->ToElement()->Parent()->ToElement()->Parent()->ToElement()->Attribute("name"),
                    device->Parent()->ToElement()->Parent()->ToElement()->Attribute("name"),
                    device->ToElement()->Attribute("name"));
        }
        else
        {
            snprintf(buff, sizeof(buff), "%s | %s | %s",
                    device->Parent()->ToElement()->Parent()->ToElement()->Attribute("name"),
                    device->Parent()->ToElement()->Attribute("position"),
                    device->ToElement()->Attribute("name"));

            // If it's a wireless device, get the serial number so we can use it for the RSSI and battery level mappings
            if (device->Parent()->ToElement()->Attribute("serialnumber") != nullptr)
            {
                const auto serialNumber_raw = std::string(device->Parent()->ToElement()->Attribute("serialnumber")).substr(3);
                serialNumber = std::strtoul(serialNumber_raw.c_str(), NULL, 16);
            }
        }
        _eSwitchType device;
        switch (deviceType)
        {
            case sSwitchIHCFBOutput:
            case sSwitchIHCInput:
                device = STYPE_Contact;
                break;
            case sSwitchIHCFBInput:
                device = STYPE_PushOn;
                break;
            case sSwitchIHCDimmer:
                device = STYPE_Dimmer;
                break;
            case sSwitchIHCOutput:
                device = STYPE_OnOff;
                break;
            default:
                return;
        }

        m_sql.safe_query(
                "INSERT INTO DeviceStatus (HardwareID, DeviceID, Type, SubType, SwitchType, SignalLevel, BatteryLevel, Name, nValue, AddjValue, AddjValue2, sValue, Options) "
                "VALUES (%d, '%q', %d, %d, %d, 12, 255, '%q', 0, '', '', '', '%lld')",
                m_HwdID, sid, pTypeGeneralSwitch, deviceType, STYPE_OnOff, buff, 1.0, 1.0, serialNumber);
    }
    else
    {
        // TODO: Remove when merging to master
        // Device already exists - add serialnumber
        // If it's a wireless device, get the serial number so we can use it for the RSSI and battery level mappings
        if (device->Parent()->ToElement()->Attribute("serialnumber") != nullptr)
        {
            std::string const serialNumber_raw = std::string(device->Parent()->ToElement()->Attribute("serialnumber")).substr(3);
            serialNumber = std::strtoul(serialNumber_raw.c_str(), NULL, 16);
            m_sql.safe_query("UPDATE DeviceStatus SET Options = '%lld' WHERE HardwareID = '%d' AND DeviceID = '%q'", serialNumber, m_HwdID, sid);

        }
    }
}

void CLKIHC::iterateDevices(const TiXmlNode* deviceNode)
{
    bool isFunctionBlock = false;
    unsigned char deviceType = -1;

    if (strcmp(deviceNode->Value(), "airlink_dimming") == 0)
    {
        deviceType = sSwitchIHCDimmer;
    }
    else if ((strcmp(deviceNode->Value(), "airlink_relay") == 0) || (strcmp(deviceNode->Value(), "dataline_output") == 0))
    {
        deviceType = sSwitchIHCOutput;
    }
    else if ((strcmp(deviceNode->Value(), "airlink_input") == 0) || (strcmp(deviceNode->Value(), "dataline_input") == 0))
    {
        deviceType = sSwitchIHCInput;
    }
    else if (strcmp(deviceNode->Value(), "resource_input") == 0)
    {
        deviceType = sSwitchIHCInput;
        isFunctionBlock = true;
    }
    else if (strcmp(deviceNode->Value(), "resource_output") == 0)
    {
        deviceType = sSwitchIHCOutput;
        isFunctionBlock = true;
    }
    else if (strcmp(deviceNode->Value(), "resource_temperature") == 0)
    {
        deviceType = sSwitchIHCOutput;
        isFunctionBlock = true;
    }

    if (deviceType != -1)
    {
        addDeviceIfNotExists(deviceNode, deviceType, isFunctionBlock);
    }

    for (const TiXmlNode* node = deviceNode->FirstChild(); node; node = node->NextSibling())
    {
        iterateDevices(node);
    }
}

void CLKIHC::GetDevicesFromController()
{
    if (ihcC->CONNECTED != ihcC->connectionState) {
        ihcC->openConnection();
    }
    TiXmlDocument doc = ihcC->loadProject();

    TinyXPath::xpath_processor processor ( doc.RootElement(), "/utcs_project/groups/*/*[self::functionblock or self::product_dataline or self::product_airlink]");

    unsigned const numberOfDevices = processor.u_compute_xpath_node_set();

    for (int i = 0; i < numberOfDevices; i++)
    {
        TiXmlNode* thisNode = processor.XNp_get_xpath_node(i);
        iterateDevices(thisNode);
    }
}

std::string CLKIHC::getValue( TiXmlElement* const a, std::string const t)
{
    TinyXPath::xpath_processor proc(a, t.c_str());
    if (1 == proc.u_compute_xpath_node_set())
    {
        TiXmlNode* thisNode = proc.XNp_get_xpath_node(0);
        std::string res = thisNode->ToElement()->GetText();
        return res;
    }
    return "";
}

bool CLKIHC::UpdateBatteryAndRSSI()
{
#ifdef _DEBUG
    _log.Log(LOG_STATUS, "LK IHC: Updating battery and RSSI levels");
#endif
    TiXmlDocument const RFandRSSIinfo =  ihcC->getRF();
    TinyXPath::xpath_processor processor ( RFandRSSIinfo.RootElement(), "/SOAP-ENV:Envelope/SOAP-ENV:Body/ns1:getDetectedDeviceList1/ns1:arrayItem");
    processor.u_compute_xpath_node_set(); // <-- this is important. It executes the Xpath expression
    if (processor.XNp_get_xpath_node(0)->FirstChild() != nullptr)
    {
        for (unsigned int i = 0; i < processor.u_compute_xpath_node_set(); i++)
        {
            TiXmlNode* const thisNode = processor.XNp_get_xpath_node(i);
            TiXmlElement * const res = thisNode->FirstChild()->ToElement();
            int const batteryLevel = boost::lexical_cast<int>(getValue(res, "/ns1:batteryLevel"));
            int const signalStrength = (int)(boost::lexical_cast<int>(getValue(res, "/ns1:signalStrength"))>>2);
            IhcDeviceSerial serialNumber   = boost::lexical_cast<unsigned long>(getValue(res, "/ns1:serialNumber"));

            auto receiversEntries = IhcDeviceSerialToDeviceID.equal_range(serialNumber);
            for (auto receiverEntry = receiversEntries.first; receiverEntry != receiversEntries.second; ++receiverEntry)
            {
                DeviceCache[receiverEntry->second].Battery = batteryLevel*100;
                DeviceCache[receiverEntry->second].RSSI = signalStrength;
            }
            m_sql.safe_query("UPDATE DeviceStatus SET BatteryLevel=%d, SignalLevel=%d WHERE (HardwareID==%d) AND (Options==%lld)",
                    (batteryLevel * 100), signalStrength, m_HwdID, serialNumber);
        }
        return true;
    }
    return false;
}

//Webserver helpers
namespace http {
    namespace server {

        void CWebServer::GetIHCProjectFromController(WebEmSession & session, const request& req, std::string & redirect_uri)
        {
            redirect_uri = "/index.html";
            if (session.rights != 2)
            {
                //No admin user, and not allowed to be here
                return;
            }

            std::string idx = request::findValue(&req, "idx");
            if (idx == "") {
                return;
            }

            int iHardwareID = atoi(idx.c_str());
            CDomoticzHardwareBase *pBaseHardware = m_mainworker.GetHardware(iHardwareID);
            if (pBaseHardware == nullptr)
            {
                return;
            }
            if (pBaseHardware->HwdType != HTYPE_IHC)
            {
                return;
            }
            CLKIHC *pHardware = reinterpret_cast<CLKIHC*>(pBaseHardware);

            try
            {
                pHardware->GetDevicesFromController();
            }
            catch (const char* msg)
            {
                _log.Log(LOG_ERROR, "LK IHC: Exception: '%s'", msg);
            }
        }

    }
}
