#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include "mystrlib.h"
#include "DataLogger.h"
#include "Config.h"
#include "TemperatureFormats.h"
#include "BrewPiProxy.h"
#include "ExternalData.h"
extern BrewPiProxy brewPi;

#define GSLOG_JSON_BUFFER_SIZE 256
#define MAX_GSLOG_CONFIG_LEN 1024

#define KeyBeerTemp "beerTemp"
#define KeyBeerSet "beerSet"
#define KeyFridgeTemp "fridgeTemp"
#define KeyFridgeSet "fridgeSet"
#define KeyRoomTemp "roomTemp"
#define KeyGravity "gravity"
#define KeyPlato "plato"
#define KeyAuxTemp "auxTemp"
#define KeyVoltage "voltage"
#define KeyTilt  "tilt"

size_t DataLogger::printFloat(char* buffer,float value,int precision,bool valid)
{
	if(valid){
		return sprintFloat(buffer,value,precision);
	}else{
		strcpy(buffer,"null"); // ubidots.com doesn't accept "null" as values.
		return 4;
	}
}

size_t DataLogger::dataSprintf(char *buffer,const char *format)
{
	uint8_t state, mode;
	float beerSet,fridgeSet;
	float beerTemp,fridgeTemp,roomTemp;

	brewPi.getAllStatus(&state,&mode,& beerTemp,& beerSet,& fridgeTemp,& fridgeSet,& roomTemp);

	int i=0;
	size_t d=0;
	for(i=0;i< (int) strlen(format);i++){
		char ch=format[i];
		if( ch == '%'){
			i++;
			ch=format[i];
			if(ch == '%'){
				buffer[d++]=ch;
			}else if(ch == 'b'){
				d += printFloat(buffer+d,beerTemp,1,IS_FLOAT_TEMP_VALID(beerTemp));
			}else if(ch == 'B'){
				d += printFloat(buffer+d,beerSet,1,IS_FLOAT_TEMP_VALID(beerSet));
			}else if(ch == 'f'){
				d += printFloat(buffer+d,fridgeTemp,1,IS_FLOAT_TEMP_VALID(fridgeTemp));
			}else if(ch == 'F'){
				d += printFloat(buffer+d,fridgeSet,1,IS_FLOAT_TEMP_VALID(fridgeSet));
			}else if(ch == 'r'){
				d += printFloat(buffer+d,roomTemp,1,IS_FLOAT_TEMP_VALID(roomTemp));
			}else if(ch == 'g'){
				float sg=externalData.gravity();
				d += printFloat(buffer+d,sg,4,IsGravityValid(sg));
			}else if(ch == 'p'){
				float sg=externalData.plato();
				d += printFloat(buffer+d,sg,2,IsGravityValid(sg));
			}else if(ch == 'v'){
				float vol=externalData.deviceVoltage();
				d += printFloat(buffer+d,vol,1,IsVoltageValid(vol));
			}else if(ch == 'a'){
				float at=externalData.auxTemp();
				d += printFloat(buffer+d,at,1,IS_FLOAT_TEMP_VALID(at));
			}else if(ch == 't'){
				float tilt=externalData.tiltValue();
				d += printFloat(buffer+d,tilt,2,true);
			}else if(ch == 'u'){
				d += sprintInt(buffer+d, externalData.lastUpdate());
			}else{
				// wrong format
				return 0;
			}
		}else{
			buffer[d++]=ch;
		}
	}// for each char

	buffer[d]='\0';
	return d;
}

void DataLogger::loop(time_t now)
{
	if(!_loggingInfo->enabled) return;

	if((now - _lastUpdate) < _loggingInfo->period) return;

	sendData();
	_lastUpdate=now;
}

int _copyName(char *buf,char *name,bool concate)
{
	char *ptr=buf;
	if(name ==NULL) return 0;
	if(concate){
		*ptr='&';
		ptr++;
	}
	int len=strlen(name);
	strcpy(ptr,name);
	ptr+=len;
	*ptr = '=';
	ptr++;
	return (ptr - buf);
}

int copyTemp(char* buf,char* name,float value, bool concate)
{
	int n;
	if((n = _copyName(buf,name,concate))!=0){
		if(IS_FLOAT_TEMP_VALID(value)){
			n += sprintFloat(buf + n ,value,2);
		}else{
			strcpy(buf + n,"null");
			n += 4;
		}

	}
	return n;
}


size_t DataLogger::nonNullJson(char* buffer,size_t size)
{
	const int JSON_BUFFER_SIZE = JSON_OBJECT_SIZE(15);
	DynamicJsonBuffer jsonBuffer(JSON_BUFFER_SIZE);
	JsonObject& root = jsonBuffer.createObject();

	uint8_t state, mode;
	float beerSet,fridgeSet;
	float beerTemp,fridgeTemp,roomTemp;

	brewPi.getAllStatus(&state,&mode,& beerTemp,& beerSet,& fridgeTemp,& fridgeSet,& roomTemp);

	if(IS_FLOAT_TEMP_VALID(beerTemp)) root[KeyBeerTemp] = beerTemp;
	if(IS_FLOAT_TEMP_VALID(beerSet)) root[KeyBeerSet] = beerSet;
	if(IS_FLOAT_TEMP_VALID(fridgeTemp)) root[KeyFridgeTemp] = fridgeTemp;
	if(IS_FLOAT_TEMP_VALID(fridgeSet)) root[KeyFridgeSet] = fridgeSet;
	if(IS_FLOAT_TEMP_VALID(roomTemp)) root[KeyRoomTemp] = roomTemp;

	float sg=externalData.gravity();
	if(IsGravityValid(sg)){
		root[KeyGravity] = sg;
		root[KeyPlato] = externalData.plato();
	}

	// iSpindel data
	float vol=externalData.deviceVoltage();
	if(IsVoltageValid(vol)){
		 root[KeyVoltage] = vol;
		float at=externalData.auxTemp();
		if(IS_FLOAT_TEMP_VALID(at)) root[KeyAuxTemp] = at;
		float tilt=externalData.tiltValue();
		root[KeyTilt]=tilt;
	}

	return root.printTo(buffer,size);
}

#define BUFFERSIZE 512

void DataLogger::sendData(void)
{
	char data[BUFFERSIZE];
	int len=0;

	if(_loggingInfo->service == ServiceNonNullJson){
		len = nonNullJson(data,BUFFERSIZE);
	}else{
		len =dataSprintf(data,_loggingInfo->format);
	}

	if(len==0){
		DBG_PRINTF("Invalid format\n");
		return;
	}

	DBG_PRINTF("url=\"%s\"\n",_loggingInfo->url);
	DBG_PRINTF("data= %d, \"%s\"\n",len,data);

	int code;
	HTTPClient _http;
  	_http.setUserAgent(F("ESP8266"));

	DBG_PRINTF("[HTTP] %d...\n",_loggingInfo->method);
	DBG_PRINTF("Content-Type:\"%s\"\n", _loggingInfo->contentType);
	if(_loggingInfo->method == mHTTP_POST
		|| _loggingInfo->method== mHTTP_PUT ){
		// post

 		_http.begin(_loggingInfo->url);
 		if(_loggingInfo->contentType){
  			_http.addHeader("Content-Type", _loggingInfo->contentType);
 		}else{
  			_http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  		}
    // start connection and send HTTP header
    	code = _http.sendRequest((_loggingInfo->method == mHTTP_POST)? "POST":"PUT",(uint8_t*)data,len);
    }else{
 		_http.begin(String(_loggingInfo->url) + String("?") + String(data));

    	code = _http.GET();
    }

    if(code <= 0) {
        DBG_PRINTF("HTTP error: %s\n", _http.errorToString(code).c_str());
        _http.end();
        return;
    }
      // HTTP header has been send and Server response header has been handled
    DBG_PRINTF("[HTTP] result code: %d\n", code);
    if(code == HTTP_CODE_OK){

    }else if((code / 100) == 3 && _http.hasHeader("Location")){
      String location=_http.header("Location");
      DBG_PRINTF("redirect:%s\n",location.c_str());
    }else{
      DBG_PRINTF("error, unhandled code:%d",code);
    }
    String output=_http.getString();
    DBG_PRINTF("output:\n%s\n",output.c_str());
	_http.end();
}