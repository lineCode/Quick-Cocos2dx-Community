// Copyright 2016 KeNan Liu
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <memory.h>

#include "cocos/audio/RDAudio.h"
#include "cocos/audio/RDAudioOgg.h"
#include "cocos2d.h"

// singleton stuff
RDAudio *RDAudio::s_instant = nullptr;

RDAudio::RDAudio()
: _device(nullptr)
, _context(nullptr)
, _thread(nullptr)
, _needQuit(false)
, _asyncRefCount(0)
{
}

RDAudio::~RDAudio()
{
    if (_context) {
        alcDestroyContext(_context);
    }
    if (_device) {
        alcCloseDevice(_device);
    }
    if (_thread) {
        _thread->join();
        delete _thread;
    }
}

RDAudio *RDAudio::getInstance()
{
    if (!s_instant) {
        s_instant = new (std::nothrow) RDAudio();
        s_instant->init();
    }
    
    return s_instant;
}

void RDAudio::destroyInstance()
{
    if (s_instant) {
        s_instant->waitForQuit();
        delete s_instant;
        s_instant = nullptr;
    }
}

void RDAudio::waitForQuit()
{
    // notify sub thread to quick
    _needQuit = true;
    _sleepCondition.notify_one();
    if (_thread) {
        _thread->join();
    }
}

void RDAudio::init(void)
{
    // Initialization device
    if (!_device) {
        // select the "preferred device"
        _device = alcOpenDevice(NULL);
        if (_device) {
            _context = alcCreateContext(_device, NULL);
            alcMakeContextCurrent(_context);
        }
        // Check for EAX 2.0 support
        ALboolean g_bEAX = alIsExtensionPresent("EAX2.0");
        if (g_bEAX == false) {
            cocos2d::log("Error: RDAudio_init can't support EAX2.0");
        }
        alGetError(); // clear error code
        
        _thread = new std::thread(&RDAudio::threadLoop, this);
    }
}

void RDAudio::threadLoop()
{
    AsyncStruct *asyncStruct = nullptr;
    
    while (true) {
        _inMutex.lock();
        if (_inQueue.empty()) {
            _inMutex.unlock();
            if (_needQuit) {
                break;
            }
            std::unique_lock<std::mutex> lk(_sleepMutex);
            _sleepCondition.wait(lk);
            continue;
        }
        
        asyncStruct = _inQueue.front();
        _inQueue.pop();
        _inMutex.unlock();
        
        // decode
        cocos2d::Data data = cocos2d::FileUtils::getInstance()->getDataFromFile(asyncStruct->filename);
        if (data.getSize() > 0) {
            int rtn = decodeOgg(&data,
                                &asyncStruct->pcmData,
                                &asyncStruct->channels,
                                &asyncStruct->rate,
                                &asyncStruct->size);
            if (rtn < 0) {
                if (asyncStruct->pcmData) {
                    free(asyncStruct->pcmData);
                    // set to NULL for main thread error check
                    asyncStruct->pcmData = NULL;
                }
            }
        }
        
        // add to outQueue
        _outMutex.lock();
        _outQueue.push(asyncStruct);
        _outMutex.unlock();
    }
}

void RDAudio::scheduleLoop(float)
{
    AsyncStruct *asyncStruct = nullptr;
    
    _outMutex.lock();
    if (_outQueue.empty())
    {
        _outMutex.unlock();
        return;
    }
    
    asyncStruct = _outQueue.front();
    _outQueue.pop();
    _outMutex.unlock();
    
    // create OpenAL buffer
    ALuint bufferID = 0;
    if (asyncStruct->pcmData) {
        alGenBuffers(1, &bufferID);
        if (alGetError() != AL_NO_ERROR) {
            cocos2d::log("Error: RDAudio_LoadFile can't gen OpenAL Buffer");
        } else {
            ALenum format = (asyncStruct->channels == 1) ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16;
            alBufferData(bufferID, format, asyncStruct->pcmData, asyncStruct->size, asyncStruct->rate);
            if (alGetError() != AL_NO_ERROR) {
                cocos2d::log("Error: RDAudio_LoadFile alBufferData Fail");
                alDeleteBuffers(1, &bufferID);
                bufferID = 0;
            }
        }
    }
    // callback to lua
    asyncStruct->cb(asyncStruct->funcID, bufferID);
    // free memory
    delete asyncStruct;
    
    // remove task in main thread
    --_asyncRefCount;
    if (0 == _asyncRefCount)
    {
        cocos2d::Director::getInstance()->getScheduler()->unschedule(CC_SCHEDULE_SELECTOR(RDAudio::scheduleLoop), this);
    }
}

void RDAudio::loadFileAsyn(const char *filename, int funcID, AudioCallback cb)
{
    // add task in main thread
    if (0 == _asyncRefCount) {
        cocos2d::Director::getInstance()->getScheduler()->schedule(CC_SCHEDULE_SELECTOR(RDAudio::scheduleLoop), this, 0, false);
    }
    ++_asyncRefCount;
    
    // add task in sub thread
    AsyncStruct *asyncStruct = new (std::nothrow) AsyncStruct(filename, funcID, cb);
    _inMutex.lock();
    _inQueue.push(asyncStruct);
    _inMutex.unlock();
    
    // weak up sub thread
    _sleepCondition.notify_one();
}
