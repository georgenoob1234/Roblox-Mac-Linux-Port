/*
This file is part of Darling.

Copyright (C) 2020 Lubos Dolezel

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

#include "AUHAL.h"
#include "CoreAudio/HostClock.h"
#include <iostream>
#include <vector>

#pragma GCC visibility push(default)
AUDIOCOMPONENT_ENTRY(AUOutputBaseFactory, AUHAL);
#pragma GCC visibility pop

enum {
	kOutputBus = 0,
	kInputBus
};

AUHAL::AUHAL(AudioComponentInstance inInstance, bool supportRecording)
: AUBase(inInstance, supportRecording ? 2 : 1, supportRecording ? 2 : 1)
{
	UInt32 propSize = sizeof(AudioDeviceID);
	AudioHardwareGetProperty(kAudioHardwarePropertyDefaultOutputDevice, &propSize, &m_outputDevice);
	AudioHardwareGetProperty(kAudioHardwarePropertyDefaultInputDevice, &propSize, &m_inputDevice);
}

AUHAL::~AUHAL()
{
	Stop();
}

bool AUHAL::CanScheduleParameters() const
{
	return true;
}

bool AUHAL::StreamFormatWritable(AudioUnitScope scope, AudioUnitElement element)
{
	return !m_running && !(scope == kAudioUnitScope_Input && element == kInputBus);
}

const CAStreamBasicDescription& AUHAL::GetStreamFormat(AudioUnitScope scope, AudioUnitElement element)
{
	if (scope == kAudioUnitScope_Input && element == kInputBus)
	{
		GetInput(element); // Validate that this unit supports recording.
		UInt32 size = sizeof(m_inputFormat);
		OSStatus status = AudioDeviceGetProperty(m_inputDevice, 0, true,
			kAudioDevicePropertyStreamFormat, &size, &m_inputFormat);
		if (status) COMPONENT_THROW(status);
		return m_inputFormat;
	}
	return AUBase::GetStreamFormat(scope, element);
}

OSStatus AUHAL::Version()
{
	return 1;
}

// These canonical PCM layouts need no channel reordering in the Pulse stream.
// Reject other layouts rather than accepting a map the renderer cannot honor.
UInt32 AUHAL::GetChannelLayoutTags(AudioUnitScope scope,AudioUnitElement element,AudioChannelLayoutTag* tags) {
 auto channels=GetIOElement(scope,element)->GetStreamFormat().NumberChannels();
 if(channels!=1&&channels!=2)return 0;
 if(tags)*tags=channels==1?kAudioChannelLayoutTag_Mono:kAudioChannelLayoutTag_Stereo;
 return 1;
}
UInt32 AUHAL::GetAudioChannelLayout(AudioUnitScope scope,AudioUnitElement element,AudioChannelLayout* out,Boolean& writable) {
 AudioChannelLayoutTag tag;writable=true;
 if(!GetChannelLayoutTags(scope,element,&tag))return 0;
 if(out){out->mChannelLayoutTag=tag;out->mChannelBitmap=0;out->mNumberChannelDescriptions=0;}
 return offsetof(AudioChannelLayout,mChannelDescriptions);
}
OSStatus AUHAL::SetAudioChannelLayout(AudioUnitScope scope,AudioUnitElement element,const AudioChannelLayout* layout) {
 if(m_running)return kAudioUnitErr_CannotDoInCurrentContext;
 AudioChannelLayoutTag tag;
 if(!GetChannelLayoutTags(scope,element,&tag)||layout->mChannelLayoutTag!=tag||layout->mChannelBitmap||layout->mNumberChannelDescriptions)
  return kAudioUnitErr_InvalidPropertyValue;
 return noErr; // The validated layout is derived from the current PCM format.
}

// Provide data from the microphone
OSStatus AUHAL::Render(AudioUnitRenderActionFlags& ioActionFlags, const AudioTimeStamp& inTimeStamp, UInt32 inNumberFrames)
{
	std::unique_lock<std::mutex> lk(m_dataAvailableMutex);

	m_dataAvailableCV.wait(lk, [=]{ return m_dataAvailable; });

	AudioBufferList& abl = GetOutput(kInputBus)->GetBufferList();

	// TODO: Prepare for non-interleaved audio
	UInt32 howMuch = std::min<UInt32>(abl.mBuffers[0].mDataByteSize, m_bufferUsed);
	memcpy(abl.mBuffers[0].mData, m_buffer.get(), howMuch);
	abl.mBuffers[0].mDataByteSize = howMuch;

	return noErr;
}

OSStatus AUHAL::Start()
{
	if (m_running)
		return noErr;
	Reset(kAudioUnitScope_Global, 0);

	if (m_enableOutput)
	{
		// std::cout << "Output is enabled, starting playback\n";
		AudioDeviceCreateIOProcID(m_outputDevice, playbackCallback, this, &m_outputProcID);

		// m_auhalData.open("/tmp/auhal.raw", std::ios_base::binary | std::ios_base::out);

		const CAStreamBasicDescription& desc = GetStreamFormat(kAudioUnitScope_Input, kOutputBus);
		AudioDeviceSetProperty(m_outputDevice, nullptr, 0, false, kAudioDevicePropertyStreamFormat, sizeof(AudioStreamBasicDescription), &desc);
		AudioDeviceStart(m_outputDevice, m_outputProcID);
	}
	if (m_enableInput)
	{
		AudioDeviceCreateIOProcID(m_inputDevice, recordCallback, this, &m_inputProcID);
		const CAStreamBasicDescription& desc = GetStreamFormat(kAudioUnitScope_Output, kInputBus);
		AudioDeviceSetProperty(m_inputDevice, nullptr, 0, true, kAudioDevicePropertyStreamFormat, sizeof(AudioStreamBasicDescription), &desc);
		AudioDeviceStart(m_inputDevice, m_inputProcID);
	}

	m_running = m_enableOutput || m_enableInput;
	return noErr;
}

OSStatus AUHAL::Stop()
{
	if (m_outputProcID)
	{
		AudioDeviceStop(m_outputDevice, m_outputProcID);
		AudioDeviceDestroyIOProcID(m_outputDevice, m_outputProcID);
		m_outputProcID=nullptr;
	}
	if (m_inputProcID)
	{
		AudioDeviceStop(m_inputDevice, m_inputProcID);
		AudioDeviceDestroyIOProcID(m_inputDevice, m_inputProcID);
		m_inputProcID=nullptr;
	}

	m_running=false;
	return noErr;
}

OSStatus AUHAL::SetProperty(AudioUnitPropertyID inID, AudioUnitScope inScope, AudioUnitElement inElement, const void* inData, UInt32 inDataSize)
{
	switch (inID)
	{
		case kAudioOutputUnitProperty_SetInputCallback:
		{
			ca_require(inDataSize == sizeof(AURenderCallbackStruct), InvalidPropertyValue);
			const AURenderCallbackStruct* cb = static_cast<const AURenderCallbackStruct*>(inData);
			
			m_outputAvailableCb = *cb;

			PropertyChanged(inID, inScope, inElement);
			return noErr;
		}
		case kAudioOutputUnitProperty_EnableIO:
		{
			ca_require(inDataSize == sizeof(UInt32), InvalidPropertyValue);

			const bool enable = *((const UInt32*) inData);

			if (inElement == kOutputBus)
			{
				m_enableOutput = enable;
				PropertyChanged(inID, inScope, inElement);
			}
			else if (inElement == kInputBus)
			{
				m_enableInput = enable;
				PropertyChanged(inID, inScope, inElement);
			}
			else
				return kAudioUnitErr_InvalidElement;

			return noErr;
		}
		case kAudioOutputUnitProperty_CurrentDevice:
		{
			ca_require(inDataSize == sizeof(AudioDeviceID), InvalidPropertyValue);
			const AudioDeviceID* dev = static_cast<const AudioDeviceID*>(inData);

            // HAL CurrentDevice uses global scope / element 0 even for an
            // input-only device. Inspect its real directions before selecting.
            if(inElement>kInputBus)return kAudioUnitErr_InvalidElement;
            auto hasChannels=[&](AudioObjectPropertyScope scope) {
                AudioObjectPropertyAddress a={kAudioDevicePropertyStreamConfiguration,scope,0};
                UInt32 n=0;
                if(AudioObjectGetPropertyDataSize(*dev,&a,0,nullptr,&n)||n<offsetof(AudioBufferList,mBuffers))return false;
                std::vector<unsigned char> bytes(n);
                if(AudioObjectGetPropertyData(*dev,&a,0,nullptr,&n,bytes.data()))return false;
                auto* list=reinterpret_cast<AudioBufferList*>(bytes.data());
                for(UInt32 i=0;i<list->mNumberBuffers;i++)if(list->mBuffers[i].mNumberChannels)return true;
                return false;
            };
            bool input=hasChannels(kAudioObjectPropertyScopeInput),output=hasChannels(kAudioObjectPropertyScopeOutput);
            if((!input&&!output)||(inElement==kInputBus&&!input))return kAudioUnitErr_InvalidPropertyValue;
            bool running=m_running;if(running)Stop();
            if(input)m_inputDevice=*dev;
            if(output&&inElement==kOutputBus)m_outputDevice=*dev;
            PropertyChanged(inID,inScope,inElement);
            if(running)return Start();
			return noErr;
		}
	}
	return AUBase::SetProperty(inID, inScope, inElement, inData, inDataSize);
InvalidPropertyValue:
	return kAudioUnitErr_InvalidPropertyValue;
}

OSStatus AUHAL::GetPropertyInfo(AudioUnitPropertyID inID, AudioUnitScope inScope, AudioUnitElement inElement, UInt32& outDataSize, Boolean& outWritable)
{
	switch (inID)
	{
		case kAudioOutputUnitProperty_SetInputCallback:
		{
			outDataSize = sizeof(AURenderCallbackStruct);
			outWritable = true;
			break;
		}
		case kAudioOutputUnitProperty_EnableIO:
		{
			outDataSize = sizeof(UInt32);
			outWritable = true;
			return noErr;
		}
		case kAudioOutputUnitProperty_HasIO:
		{
			outDataSize = sizeof(UInt32);
			outWritable = false;
			return noErr;
		}
		case kAudioOutputUnitProperty_CurrentDevice:
		{
			outDataSize = sizeof(AudioDeviceID);
			outWritable = true;
			return noErr;
		}
	}
	return AUBase::GetPropertyInfo(inID, inScope, inElement, outDataSize, outWritable);
}

OSStatus AUHAL::GetProperty(AudioUnitPropertyID inID, AudioUnitScope inScope, AudioUnitElement inElement, void* outData)
{
	switch (inID)
	{
		case kAudioOutputUnitProperty_SetInputCallback:
		{
			memcpy(outData, &m_outputAvailableCb, sizeof(m_outputAvailableCb));
			return noErr;
		}
		case kAudioOutputUnitProperty_EnableIO:
		case kAudioOutputUnitProperty_HasIO:
		{
			if (inElement == kOutputBus)
			{
				UInt32 value = m_enableOutput;
				memcpy(outData, &value, sizeof(value));
			}
			else if (inElement == kInputBus)
			{
				UInt32 value = m_enableInput;
				memcpy(outData, &value, sizeof(value));
			}
			else
				return kAudioUnitErr_InvalidElement;
			return noErr;
		}
		case kAudioOutputUnitProperty_CurrentDevice:
		{
			if (inElement == kOutputBus)
			{
				AudioDeviceID selected=(!m_enableOutput&&m_enableInput)?m_inputDevice:m_outputDevice;
				memcpy(outData, &selected, sizeof(selected));
			}
			else if (inElement == kInputBus)
			{
				memcpy(outData, &m_inputDevice, sizeof(m_inputDevice));
			}
			else
				return kAudioUnitErr_InvalidElement;
			return noErr;
		}
	}
	return AUBase::GetProperty(inID, inScope, inElement, outData);
}

OSStatus AUHAL::playbackCallback(AudioObjectID inObjectID,
	const AudioTimeStamp* inNow, const AudioBufferList* inInputData,
	const AudioTimeStamp* inInputTime,
	AudioBufferList* outOutputData, const AudioTimeStamp* inOutputTime,
	void* inClientData)
{
	AUHAL* This = static_cast<AUHAL*>(inClientData);
	return This->doPlayback(inNow, outOutputData, inOutputTime);
}

OSStatus AUHAL::recordCallback(AudioObjectID inObjectID,
	const AudioTimeStamp* inNow, const AudioBufferList* inInputData,
	const AudioTimeStamp* inInputTime,
	AudioBufferList* outOutputData, const AudioTimeStamp* inOutputTime,
	void* inClientData)
{
	AUHAL* This = static_cast<AUHAL*>(inClientData);
	return This->doRecord(inNow, inInputData, inInputTime);
}

OSStatus AUHAL::doPlayback(const AudioTimeStamp* inNow, AudioBufferList* outOutputData, const AudioTimeStamp* inOutputTime)
{
	// std::cout << "AUHAL::DoPlayback()\n";
	if (!HasInput(0))
	{
		// std::cerr << "No connection\n";
		return kAudioUnitErr_NoConnection;
	}

	OSStatus result = noErr;
	AudioUnitRenderActionFlags flags = kAudioUnitRenderAction_PreRender;
	const CAStreamBasicDescription& desc = GetStreamFormat(kAudioUnitScope_Input, kOutputBus);

	UInt32 nFrames = outOutputData->mBuffers[0].mDataByteSize / desc.mBytesPerFrame;
	result = GetInput(kOutputBus)->PullInputWithBufferList(flags, *inNow, kOutputBus, nFrames, outOutputData);

	// std::cout << "Pull result: " << result << std::endl;
	// std::cout << "Bytes: " << outOutputData->mBuffers[0].mDataByteSize << std::endl;
	// m_auhalData.write((char*) outOutputData->mBuffers[0].mData, outOutputData->mBuffers[0].mDataByteSize);
	// m_auhalData.flush();

	return result;
}

OSStatus AUHAL::doRecord(const AudioTimeStamp* inNow, const AudioBufferList* inInputData, const AudioTimeStamp* inInputTime)
{
	const auto& desc = GetStreamFormat(kAudioUnitScope_Output, kInputBus);
	const auto& input = inInputData->mBuffers[0];
	size_t maxBytes = size_t(GetMaxFramesPerSlice()) * desc.mBytesPerFrame;
	if (!maxBytes) return kAudioUnitErr_TooManyFramesToProcess;
	// Pulse packets can exceed MaximumFramesPerSlice, especially for mono PCM.
	for (size_t offset = 0; offset < input.mDataByteSize; )
	{
		size_t bytes = std::min(maxBytes, input.mDataByteSize - offset);
		std::unique_lock<std::mutex> lk(m_dataAvailableMutex);
		if (m_bufferSize < bytes)
		{
			m_buffer.reset(new uint8_t[bytes]);
			m_bufferSize = bytes;
		}
		m_bufferUsed = bytes;
		memcpy(m_buffer.get(), static_cast<const uint8_t*>(input.mData) + offset, bytes);
		m_dataAvailable = true;
		lk.unlock();
		m_dataAvailableCV.notify_one();

		if (m_outputAvailableCb.inputProc)
		{
			AudioTimeStamp time = *inInputTime;
			auto frames = offset / desc.mBytesPerFrame;
			time.mSampleTime += frames;
			time.mHostTime += AudioConvertNanosToHostTime(UInt64(frames * 1e9 / desc.mSampleRate));
			AudioUnitRenderActionFlags flags = 0;
			m_outputAvailableCb.inputProc(m_outputAvailableCb.inputProcRefCon, &flags,
				&time, kInputBus, bytes / desc.mBytesPerFrame, nullptr);
		}
		offset += bytes;
	}

	return noErr;
}

// AUDispatch.cpp doesn't implement dispatch code for AudioOutputUnits
OSStatus AUHAL::ComponentEntryDispatch(ComponentParameters *params, AUHAL *This)
{
	if (This == NULL) return kAudio_ParamError;

	OSStatus result = noErr;

	switch (params->what)
	{
		case kComponentCanDoSelect:
			switch (GetSelectorForCanDo(params))
			{
				case kAudioOutputUnitStartSelect:
				case kAudioOutputUnitStopSelect:
					return 1;
			}
			break;
		case kAudioOutputUnitStartSelect:
		{
			CAMutex::Locker lock(This->GetMutex());
			return This->Start();
		}
		case kAudioOutputUnitStopSelect:
		{
			CAMutex::Locker lock(This->GetMutex());
			return This->Stop();
		}
	}

	return AUBase::ComponentEntryDispatch(params, This);
}

bool AUHAL::ValidFormat(AudioUnitScope,AudioUnitElement,const CAStreamBasicDescription& f) {
 // Pulse streams are interleaved. The AUBase default requires planar float,
 // which this HAL never implemented and previously accepted incorrectly.
 if(f.mFormatID!=kAudioFormatLinearPCM || (f.mFormatFlags&kAudioFormatFlagIsNonInterleaved) ||
    f.mSampleRate<1 || f.mSampleRate>384000 || !f.mChannelsPerFrame || f.mChannelsPerFrame>32 ||
    f.mFramesPerPacket!=1 || f.mBytesPerPacket!=f.mBytesPerFrame ||
    f.mBytesPerFrame!=f.mChannelsPerFrame*f.mBitsPerChannel/8)return false;
 if(f.mFormatFlags&kAudioFormatFlagIsFloat)return f.mBitsPerChannel==32;
 return f.mBitsPerChannel==8 || f.mBitsPerChannel==16 || f.mBitsPerChannel==32 ||
        (f.mBitsPerChannel==24 && (f.mFormatFlags&kAudioFormatFlagIsSignedInteger));
}
