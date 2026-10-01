/*
This file is part of Darling.

Copyright (C) 2015-2020 Lubos Dolezel

Darling is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Darling is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Darling.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <CoreAudio/AudioHardware.h>
#include "AudioHardwareImpl.h"
#include "pulse/AudioHardwareImplPA.h"
#include "pulse/AudioHardwareImplPAOutput.h"
#include "pulse/AudioHardwareImplPAInput.h"
#include <CarbonCore/MacErrors.h>
#include <dispatch/dispatch.h>
#include <memory>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include "stub.h"
#include "pulse/AudioHardwareStreamPAInput.h"
#include "pulse/AudioHardwareStreamPAOutput.h"

static std::unordered_map<AudioObjectID, std::unique_ptr<AudioHardwareImpl>> g_objects;

#include "HostDevices.h"

static void initObjects()
{
	static dispatch_once_t once;
	dispatch_once(&once, ^{
		// TODO: Or ALSA
		g_objects.insert(std::make_pair(kAudioObjectSystemObject, std::make_unique<AudioHardwareImplPA>(kAudioObjectSystemObject)));
		g_objects.insert(std::make_pair(kAudioObjectSystemObject + 1, std::make_unique<AudioHardwareImplPAOutput>(kAudioObjectSystemObject + 1)));
		g_objects.insert(std::make_pair(kAudioObjectSystemObject + 2, std::make_unique<AudioHardwareImplPAInput>(kAudioObjectSystemObject + 2)));
		g_objects.insert(std::make_pair(kAudioObjectSystemObject + 3, std::make_unique<AudioHardwareImplPAOutput>(kAudioObjectSystemObject + 3, "event")));
	});
}

static AudioHardwareImpl* GetObject(AudioObjectID objID)
{
	initObjects();
	refreshHostDevices();
	std::lock_guard<std::recursive_mutex> lock(g_objectsMutex);

	auto it = g_objects.find(objID);
	if (it == g_objects.end())
		return nullptr;

	return it->second.get();
}

static AudioHardwareImpl* GetSystemObject()
{
	return GetObject(kAudioObjectSystemObject);
}

void AudioObjectShow(AudioObjectID inObjectID)
{
	AudioHardwareImpl* obj = GetObject(inObjectID);
	if (obj)
		obj->show();
}

Boolean AudioObjectHasProperty(AudioObjectID inObjectID,
		const AudioObjectPropertyAddress* inAddress)
{
	AudioHardwareImpl* obj = GetObject(inObjectID);
	if (!obj)
		return 0;
	
	UInt32 size=0;
	return AudioObjectGetPropertyDataSize(inObjectID,inAddress,0,nullptr,&size)==noErr;
}

OSStatus AudioObjectIsPropertySettable(AudioObjectID inObjectID,
		const AudioObjectPropertyAddress* inAddress, Boolean* outIsSettable)
{
	AudioHardwareImpl* obj = GetObject(inObjectID);
	if (!obj)
		return kAudioHardwareBadObjectError;
	
 if(!inAddress||!outIsSettable)return kAudioHardwareIllegalOperationError;
 if(inObjectID==1&&inAddress->mSelector==kAudioHardwarePropertyRunLoop){*outIsSettable=true;return noErr;}
 if(inObjectID==1){UInt32 size=0;if(hostSystemProperty(inAddress,0,nullptr,&size,nullptr)==noErr){*outIsSettable=false;return noErr;}}
	return obj->isPropertySettable(inAddress, outIsSettable);
}

OSStatus AudioObjectGetPropertyDataSize(AudioObjectID inObjectID,
		const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize,
		const void* inQualifierData, UInt32* outDataSize)
{
	AudioHardwareImpl* obj = GetObject(inObjectID);
	if (!obj)
		return kAudioHardwareBadObjectError;
	
	if(!outDataSize||!inAddress)return kAudioHardwareIllegalOperationError;
	*outDataSize=0;
	return AudioObjectGetPropertyData(inObjectID,inAddress,inQualifierDataSize,inQualifierData,outDataSize,nullptr);
}

static OSStatus getAudioPropertyData(AudioObjectID inObjectID,
		const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize,
		const void* inQualifierData, UInt32* ioDataSize, void* outData)
{
	AudioHardwareImpl* obj = GetObject(inObjectID);
	if (!obj)
		return kAudioHardwareBadObjectError;
	
	if(!inAddress || !ioDataSize)return kAudioHardwareIllegalOperationError;
	if(inObjectID==kAudioObjectSystemObject) {
	 auto status=hostSystemProperty(inAddress,inQualifierDataSize,inQualifierData,ioDataSize,outData);
	 if(status!=kAudioHardwareUnknownPropertyError)return status;
	}
 if(outData) {
  UInt32 required=0;auto status=obj->getPropertyDataSize(inAddress,inQualifierDataSize,inQualifierData,&required);
  if(status)return status;
  if(*ioDataSize<required){*ioDataSize=required;return kAudioHardwareBadPropertySizeError;}
 }
	return obj->getPropertyData(inAddress, inQualifierDataSize, inQualifierData,
			ioDataSize, outData);
}

static OSStatus setAudioPropertyData(AudioObjectID inObjectID,
		const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize,
		const void* inQualifierData, UInt32 inDataSize, const void* inData)
{
	AudioHardwareImpl* obj = GetObject(inObjectID);
	if (!obj)
		return kAudioHardwareBadObjectError;
	
 if(!inAddress||!inData)return kAudioHardwareIllegalOperationError;
 if(inObjectID==kAudioObjectSystemObject&&inAddress->mSelector==kAudioHardwarePropertyRunLoop) {
  // NULL requests HAL-owned callback scheduling, already supplied by our serial
  // dispatch queues. Do not pretend to support a caller-provided CFRunLoop.
  if(inAddress->mScope!=kAudioObjectPropertyScopeGlobal||inAddress->mElement!=0)return kAudioHardwareUnknownPropertyError;
  if(inDataSize!=sizeof(CFRunLoopRef))return kAudioHardwareBadPropertySizeError;
  return *(const CFRunLoopRef*)inData==nullptr?noErr:kAudioHardwareUnsupportedOperationError;
 }
 if(inAddress->mSelector==kAudioDevicePropertyBufferSize&&inDataSize!=sizeof(UInt32))return kAudioHardwareBadPropertySizeError;
	return obj->setPropertyData(inAddress, inQualifierDataSize, inQualifierData,
			inDataSize, inData);
}

// Opt-in, bounded diagnostics contain only API metadata, never client payloads.
static void traceAudioProperty(const char* op,AudioObjectID id,const AudioObjectPropertyAddress* a,UInt32 size,OSStatus status) {
 static bool enabled=getenv("RBX_AUDIO_TRACE")!=nullptr;
 static std::atomic<unsigned> failures{0},runLoops{0};
 if(!enabled||!a)return;
 if(status ? failures.fetch_add(1)>=64 : a->mSelector!=kAudioHardwarePropertyRunLoop||runLoops.fetch_add(1)>=8)return;
 fprintf(stderr,"AUDIO %s id=%u selector=%08x scope=%08x element=%u size=%u status=%d\n",op,id,a->mSelector,a->mScope,a->mElement,size,status);
}
OSStatus AudioObjectGetPropertyData(AudioObjectID id,const AudioObjectPropertyAddress* a,UInt32 qs,const void* q,UInt32* size,void* out) {
 auto status=getAudioPropertyData(id,a,qs,q,size,out);traceAudioProperty("get",id,a,size?*size:0,status);return status;
}
OSStatus AudioObjectSetPropertyData(AudioObjectID id,const AudioObjectPropertyAddress* a,UInt32 qs,const void* q,UInt32 size,const void* in) {
 auto status=setAudioPropertyData(id,a,qs,q,size,in);traceAudioProperty("set",id,a,size,status);return status;
}

OSStatus AudioObjectAddPropertyListener(AudioObjectID inObjectID,
		const AudioObjectPropertyAddress* inAddress,
		AudioObjectPropertyListenerProc inListener, void* inClientData)
{
	AudioHardwareImpl* obj = GetObject(inObjectID);
	if (!obj)
		return kAudioHardwareBadObjectError;
	
	if(!inAddress||!inListener)return kAudioHardwareIllegalOperationError;
 std::lock_guard<std::recursive_mutex> lock(g_listenerMutex);
 for(const auto& l:g_listeners)if(l.id==inObjectID&&l.proc==inListener&&l.data==inClientData&&sameAddress(l.address,*inAddress))return noErr;
 g_listeners.push_back({inObjectID,*inAddress,inListener,inClientData});
 static dispatch_once_t once;
 dispatch_once(&once, ^{
  auto timer=dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,0,0,dispatch_get_global_queue(0,0));
  dispatch_source_set_timer(timer,dispatch_time(DISPATCH_TIME_NOW,NSEC_PER_SEC),NSEC_PER_SEC,NSEC_PER_SEC/10);
  dispatch_source_set_event_handler(timer, ^{refreshHostDevices();});dispatch_resume(timer);
 });
 return noErr;
}

OSStatus AudioObjectRemovePropertyListener(AudioObjectID inObjectID,
		const AudioObjectPropertyAddress* inAddress,
		AudioObjectPropertyListenerProc inListener, void* inClientData)
{
	AudioHardwareImpl* obj = GetObject(inObjectID);
	if (!obj)
		return kAudioHardwareBadObjectError;
	
	if(!inAddress||!inListener)return kAudioHardwareIllegalOperationError;
 std::lock_guard<std::recursive_mutex> lock(g_listenerMutex);
 g_listeners.erase(std::remove_if(g_listeners.begin(),g_listeners.end(),[&](const HostListener& l){return l.id==inObjectID&&l.proc==inListener&&l.data==inClientData&&sameAddress(l.address,*inAddress);}),g_listeners.end());
 return noErr;
}

OSStatus AudioHardwareUnload(void)
{
	return noErr;
}

OSStatus AudioHardwareCreateAggregateDevice(CFDictionaryRef, AudioObjectID* outDeviceID)
{
	STUB();
	if (outDeviceID)
		*outDeviceID = 0;
	
	return unimpErr;
}

OSStatus AudioHardwareDestroyAggregateDevice(AudioObjectID inDeviceID)
{
	STUB();
	return unimpErr;
}

OSStatus AudioHardwareGetProperty(AudioHardwarePropertyID inPropId, UInt32* ioPropertyDataSize, void* outPropertyData)
{
	if (!ioPropertyDataSize)
		return paramErr;

	return AudioDeviceGetProperty(kAudioObjectSystemObject, 0, false, inPropId, ioPropertyDataSize, outPropertyData);
}

OSStatus AudioDeviceGetProperty(AudioDeviceID inDevice, UInt32 inChannel, Boolean isInput, AudioDevicePropertyID inPropertyID, UInt32* ioPropertyDataSize, void* outPropertyData)
{
	AudioObjectPropertyAddress aopa = {
		inPropertyID,
		isInput ? kAudioDevicePropertyScopeInput : kAudioDevicePropertyScopeOutput, // kAudioObjectPropertyScopeGlobal
		kAudioObjectPropertyElementMaster
	};

	return AudioObjectGetPropertyData(inDevice, &aopa, 0, nullptr, ioPropertyDataSize, outPropertyData);
}

OSStatus AudioDeviceGetPropertyInfo(AudioDeviceID inDevice, UInt32 inChannel, Boolean isInput, AudioDevicePropertyID inPropertyID, UInt32* outSize, Boolean* outWritable)
{
	AudioObjectPropertyAddress aopa = {
		inPropertyID,
		isInput ? kAudioDevicePropertyScopeInput : kAudioDevicePropertyScopeOutput, // kAudioObjectPropertyScopeGlobal
		kAudioObjectPropertyElementMaster
	};

	OSStatus status;
	if (outSize)
	{
		status = AudioObjectGetPropertyDataSize(inDevice, &aopa, 0, nullptr, outSize);
		if (status != noErr)
			return status;
	}

	if (outWritable)
	{
		status = AudioObjectIsPropertySettable(inDevice, &aopa, outWritable);
	}
	return status;
}

OSStatus AudioDeviceSetProperty(AudioDeviceID inDevice, const AudioTimeStamp *inWhen, UInt32 inChannel,
	Boolean isInput, AudioDevicePropertyID inPropertyID, UInt32 inPropertyDataSize, const void *inPropertyData)
{
	AudioObjectPropertyAddress aopa = {
		inPropertyID,
		kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMaster
	};

	return AudioObjectSetPropertyData(inDevice, &aopa, 0, nullptr, inPropertyDataSize, inPropertyData);
}

OSStatus AudioHardwareGetPropertyInfo(AudioHardwarePropertyID inPropertyID, UInt32 *outSize, Boolean *outWritable)
{
	return AudioDeviceGetPropertyInfo(kAudioObjectSystemObject, 0, false, inPropertyID, outSize, outWritable);
}

OSStatus AudioDeviceCreateIOProcID(AudioObjectID inDevice,
		AudioDeviceIOProc inProc, void* inClientData,
		AudioDeviceIOProcID* outIOProcID)
{
	AudioHardwareImpl* obj = GetObject(inDevice);
	if (!obj)
		return kAudioHardwareBadObjectError;
	
	return obj->createIOProcID(inProc, inClientData, outIOProcID);
}

OSStatus AudioDeviceAddIOProc(AudioDeviceID inDevice, AudioDeviceIOProc inProc, void *inClientData)
{
	AudioHardwareImpl* obj = GetObject(inDevice);
	if (!obj)
		return kAudioHardwareBadObjectError;
	return obj->createIOProcID(inProc, inClientData, nullptr);
}

OSStatus AudioDeviceDestroyIOProcID(AudioObjectID inDevice,
		AudioDeviceIOProcID inIOProcID)
{
	AudioHardwareImpl* obj = GetObject(inDevice);
	if (!obj)
		return kAudioHardwareBadObjectError;
	
	return obj->destroyIOProcID(inIOProcID);
}

OSStatus AudioDeviceRemoveIOProc(AudioDeviceID inDevice, AudioDeviceIOProc inProc)
{
	AudioHardwareImpl* obj = GetObject(inDevice);
	if (!obj)
		return kAudioHardwareBadObjectError;
	
	return obj->destroyIOProcID(AudioDeviceIOProcID(inProc));
}

OSStatus AudioDeviceStart(AudioObjectID inDevice, AudioDeviceIOProcID inProcID)
{
	AudioHardwareImpl* obj = GetObject(inDevice);
	if (!obj)
		return kAudioHardwareBadObjectError;
	
	return obj->start(inProcID, nullptr, 0);
}

OSStatus AudioDeviceStartAtTime(AudioObjectID inDevice, AudioDeviceIOProcID inProcID,
		AudioTimeStamp* ioRequestedStartTime, UInt32 inFlags)
{
	AudioHardwareImpl* obj = GetObject(inDevice);
	if (!obj)
		return kAudioHardwareBadObjectError;
	
	return obj->start(inProcID, ioRequestedStartTime, inFlags);
}

OSStatus AudioDeviceStop(AudioObjectID inDevice, AudioDeviceIOProcID inProcID)
{
	AudioHardwareImpl* obj = GetObject(inDevice);
	if (!obj)
		return kAudioHardwareBadObjectError;
	
	return obj->stop(inProcID);
}

OSStatus AudioDeviceGetCurrentTime(AudioObjectID inDevice, AudioTimeStamp* outTime)
{
	AudioHardwareImpl* obj = GetObject(inDevice);
	if (!obj)
		return kAudioHardwareBadObjectError;
	
	return obj->getCurrentTime(outTime);
}

OSStatus AudioDeviceTranslateTime(AudioObjectID inDevice, const AudioTimeStamp* inTime,
		AudioTimeStamp* outTime)
{
	AudioHardwareImpl* obj = GetObject(inDevice);
	if (!obj)
		return kAudioHardwareBadObjectError;
	
	return obj->translateTime(inTime, outTime);
}

OSStatus AudioDeviceGetNearestStartTime(AudioObjectID inDevice,
		AudioTimeStamp* ioRequestedStartTime, UInt32 inFlags)
{
	AudioHardwareImpl* obj = GetObject(inDevice);
	if (!obj)
		return kAudioHardwareBadObjectError;
	
	return obj->getNearestStartTime(ioRequestedStartTime, inFlags);
}
