//
//  Fretless.c
//  AlephOne
//
//  Created by Robert Fielding on 10/14/11.
//
// This should remain a *pure* C library with no references to external libraries
// This file is special in this regard.  The higher we go up in layers, the less problematic it is
// to have dependencies.

/*
   Expected Uses:
       Once touches have been turned into integer IDs (TODO: include as a utility function),
       applications that need to turn continuous touch locations into pitches into Continuum-like
       MIDI should be trivial.
 
       Fretless should solve the hard-core of the problem involved in getting from simple touch handling
       in a platform specific way to writing MIDI bytes out to the network.  Fretless instrument MIDI messaging
       is deeply non-trivial, which is why this library is required.  Furthermore, the fact that it
       is indeed MIDI underneath should not matter.  The API could just as well be emitting a straightforward
       OSC rendering of the gestures.
 
       Tuning and scales are outside the scope of this layer; entirely outside of our scope.  Tuning should
       be a layer over top of this.  Even for an internal engine, we should be going through this layer, and simply
       embed an actual MIDI protocol synth.  This ensures that things are done exactly one way, and standardizes how
       we can select and embed a default engine to ship with internally.
 
       We also need to have no dependencies at all if we expect this to become standardized.  iOS, Android, Lua VMs, 
       hardware controllers, etc should be able to easily embed this.  So only standard C without dependencies is a
       good idea for this module.
 
       It is critical that this part of the instrument be proven correct, so it's essential that it isn't designed in
       such a way that applications constantly touch this code; resetting the time that this exact version was tested
       every time it has been changed.  There are many assertions in this code, and they should not come out unless
       they can be proven to prevent us from meeting timing constraints.
 
       Assertions are done via calls to the fail method.
 
   Behavior:
 
     1) We expect the listening synth to have multi-timbral behavior, while expecting that each channel is set up
        to have similar or identical patch setup.  Alternately, the synth could be in an "Omni" mode that correctly
        handles mult-timbral behavior with one timbre.
     2) To degrade gracefully, the hints can be setup to run on 1 channel or fewer channels than the maximum polyphony.
        In these cases, the messaging actually has to be different.  Note On/Off messages can come out in a different
        order based on note+channel conflicts.
     3) Note retrigger on excessive bend must be handled quietly as an internal detail that the client doesn't know about.
        This is the critical feature of this MIDI setup that allows the client to ignore the fact that MIDI is what actually
        happens behind the scenes.
     4) The bend rate is a setting that allows the rate that bends are set to be limited.  The API is deterministic,
        but when it's being called, the calling API can invoke tick() at a constant rate.  Bends will only be sent on
        then channel if it has passed the rate.  In this way, the caller doesn't have to take care to rate limit
        invokes to move explicitly; just to invoke tick() at a constant rate.  The user interface just sends the bends
        at the actual rate that things move (so it's up to the internals of the API to band-limit things, not the caller
        to be more clever).
 
     The point of the assertions is not so much just to check the correctness of this code, but to give
     good diagnostics when something does go wrong, especially when what went wrong was the way that the caller invoked us.
 */

//#include <stdio.h>

#include "Fretless.h"
#include "FretlessCommon.h"


#define CTXSTATE_INIT 0
#define CTXSTATE_BOOTED 1
/**
 Fingers specify which polyphony group they live in.  This controls the polyphony and legato behavior.
 */
struct Fretless_polyState
{
    int currentFingerInPolyGroup;
};

/**
 This maps to actual MIDI channels.  Channels are handled internal to this API as a purely private matter
 that the caller knows nothing about.  (Other than sending hints about the known constraints of the output MIDI synth.)
 */
struct Fretless_channelState
{
    int lastBend;
    int lastAftertouch;
    int currentFingerInChannel;
    int useCount;
};

/**
 This is the core of the state.  The goal is to combine poly,legato, widebend (note ties and retriggers), channel cycling
 in such a way that a standard multi-timbral MIDI synth can do a pretty good pitch-perfect rendition of the sound.  But we
 add NRPNs to fill in the gaps so that we can get 100% perfect gesture fidelity on the synths that we have some control over.
 
 Note the next/prev items for fingers versus poly groups and channels.
 We want to have a leader in both poly groups and channels, so we maintain a linked list.
 
 PolyGroups effectively always assign the newest created note as the leader, and need to remove fingers from the group
 when a finger goes up (not necessarily constrained to any particular order).
 
 Channels assign the newest finger as the leader as well, and also need to remove fingers arbitrarily from the list when
 they come up.
 */
struct Fretless_fingerState
{
    int isOn;
    int isSupressed;
    float samplePhase;
    int channel;
    int note;
    int bend;
    int velocity;
    int polyGroup;
    int nextFingerInPolyGroup;
    int prevFingerInPolyGroup;
    int nextFingerInChannel;
    int prevFingerInChannel;
    int visitingPolyGroup;
};

#define CHANNELMAX 16
#define POLYMAX 16
#define NOTEMAX 128
#define MIDI_ON 0x90
#define MIDI_BEND 0xE0
#define BENDCENTER 8192

#define STATECHECK(ctxp) \
if(ctxp->ctxState != CTXSTATE_BOOTED) \
{ \
    ctxp->fail("context is not booted yet"); \
}

#define FINGERCHECK(ctxp,finger) \
if(finger < 0 || finger >= FINGERMAX) \
{ \
    ctxp->fail("finger out of range %d",finger); \
} 

#define POLYCHECK(ctxp,polyGroup) \
if(polyGroup < 0 || polyGroup >= POLYMAX) \
{ \
    ctxp->fail("poly group out of range %d",polyGroup); \
}

#define FNOTECHECK(ctxp,fnote) \
if(fnote < -0.5 || fnote >= 127.5) \
{ \
    ctxp->fail("fnote %d",fnote); \
}


/**
 This entire library needs to be re-entrant since broadcasting to different locations 
 may mean that there is more than one MIDI rendition in use at the same time.
 */
struct Fretless_context
{
    struct Fretless_fingerState fingers[FINGERMAX];
    struct Fretless_channelState channels[CHANNELMAX];
    struct Fretless_polyState polys[POLYMAX];
    int ctxState;
    //Cycle through channels from here
    int lastAllocatedChannel;
    //Metadata for fingers
    int fingersDownCount;
    //For channel/note deconflicting
    int noteChannelDownCount[NOTEMAX][CHANNELMAX];
    int noteChannelDownRawBalance[NOTEMAX][CHANNELMAX];
    //Control channel cycling
    int  channelBase;
    int  channelSpan;
    int  channelBendSemis;
    int  supressBends;
    
    //Where MIDI bytes go
    void (*midiPutch)(char);    
    void (*midiFlush)();
    //Where we write fail messages. 
    int (*fail)(const char*,...);
    void* (*fretlessAlloc)(unsigned long size);
    void (*fretlessFree)(void*);
    int (*logger)(const char*,...);
    void (*passed)();
};

void Fretless_selfTest(struct Fretless_context* ctxp);

/**
   Get a context to start using the API.  We inject dependencies so that 
   there are no compile or run time libraries that are required to run against.
   This is the plan for extreme portability, and creating this module in such a way
   that it can be frozen for a very long time once it has been fully vetted.
 */
struct Fretless_context* Fretless_init(
                                        void (*midiPutch)(char), 
                                        void (*midiFlush)(),
                                        void* (*fretlessAlloc)(unsigned long),
                                        void (*fretlessFree)(void*),
                                        int (*fail)(const char*,...),
                                        void (*passed)(),
                                        int (*logger)(const char*,...)
                                        )
{
    struct Fretless_context* ctxp = fretlessAlloc(sizeof(struct Fretless_context));
    //Set some sane defaults for what boot will not set (user controlled)
    ctxp->ctxState=CTXSTATE_INIT;
    ctxp->channelSpan=8;
    ctxp->channelBase=0;
    ctxp->channelBendSemis=2;
    ctxp->supressBends=FALSE;
    //Set what the user explicitly passed in here
    ctxp->fail = fail;
    ctxp->midiPutch = midiPutch;
    ctxp->midiFlush = midiFlush;
    ctxp->fretlessAlloc = fretlessAlloc;
    ctxp->fretlessFree = fretlessFree;
    ctxp->logger = logger;
    ctxp->passed = passed;
    return ctxp;
}

void Fretless_free(struct Fretless_context* ctxp)
{
    ctxp->fretlessFree(ctxp);
}

void Fretless_reset_FingerState(struct Fretless_fingerState* fsPtr)
{
    fsPtr->isOn = FALSE;
    fsPtr->samplePhase = 0;
    fsPtr->channel = 0;
    fsPtr->note = 0;
    fsPtr->velocity = 0;
    fsPtr->bend = BENDCENTER;
    fsPtr->nextFingerInPolyGroup = NOBODY;
    fsPtr->prevFingerInPolyGroup = NOBODY;
    fsPtr->nextFingerInChannel = NOBODY;
    fsPtr->prevFingerInChannel = NOBODY;
    fsPtr->isSupressed = FALSE;
    fsPtr->visitingPolyGroup = NOBODY;
    fsPtr->polyGroup = NOBODY;
}

void Fretless_setMidiHintSupressBends(struct Fretless_context* ctxp, int supressBends)
{
    ctxp->supressBends = supressBends;
}

void Fretless_setMidiHintChannelBase(struct Fretless_context* ctxp, int base)
{
    if(base < 0 || base >= CHANNELMAX)
    {
        ctxp->fail("%d: base > 0 || base >= CHANNELMAX\n",base);
    }
    ctxp->channelBase = base;
    if(ctxp->channelBase + ctxp->channelSpan > FINGERMAX)
    {
        ctxp->channelSpan = FINGERMAX - ctxp->channelBase;
    }
}

int Fretless_getMidiHintChannelBase(struct Fretless_context* ctxp)
{
    return ctxp->channelBase;
}

void Fretless_setMidiHintChannelSpan(struct Fretless_context* ctxp, int span)
{
    if(span < 1 || span > CHANNELMAX)
    {
        ctxp->fail("%d: span < 0 || span > CHANNELMAX\n",span);
    }
    ctxp->channelSpan = span;
    if(ctxp->channelBase + ctxp->channelSpan > FINGERMAX)
    {
        ctxp->channelSpan = FINGERMAX - ctxp->channelBase;
    }
}

int Fretless_getMidiHintChannelSpan(struct Fretless_context* ctxp)
{
    return ctxp->channelSpan;
}

/**
 Call this AFTER boot to send it to the midi device!
 
 This is called at the end of boot
 */
void Fretless_setMidiHintChannelBendSemis(struct Fretless_context* ctxp, int semitones)
{
    if(semitones < 1 || semitones > 24)
    {
        ctxp->fail("%d: semitones < 1 || semitones > 24 -- MIDI spec limits to 24\n",semitones);
    }
    ctxp->channelBendSemis = semitones;
    if(ctxp->ctxState == CTXSTATE_BOOTED)
    {
        //int lsb;
        //int msb;
        //Fretless_numTo7BitNums(1223,&lsb,&msb);
        for(int c = 0; c < ctxp->channelSpan; c++)
        {
            int channel = ctxp->channelBase + c;
            ctxp->midiPutch(0xB0 + channel);
            ctxp->midiPutch(101);
            ctxp->midiPutch(0);
            ctxp->midiPutch(0xB0 + channel);
            ctxp->midiPutch(100);
            ctxp->midiPutch(0);
            ctxp->midiPutch(0xB0 + channel);
            ctxp->midiPutch(6);
            ctxp->midiPutch(semitones);        
            ctxp->midiPutch(0xB0 + channel);
            ctxp->midiPutch(38);
            ctxp->midiPutch(0);        
            ctxp->midiPutch(0xB0 + channel);
            ctxp->midiPutch(101);
            ctxp->midiPutch(127);
            ctxp->midiPutch(0xB0 + channel);
            ctxp->midiPutch(100);
            ctxp->midiPutch(127);
            //ctxp->logger("set ch%d bend width to %d semitones up/down\n",channel,semitones);
        }
    }
}

int Fretless_getMidiHintChannelBendSemis(struct Fretless_context* ctxp)
{
    return ctxp->channelBendSemis;
}

/**
 Must call this before anything else is callable
 This *can* be called at any time immediately after flush to quick reboot this subsystem.
 
 These should have been called first:
 
    Fretless_setMidiHintChannelBase
    Fretless_setMidiHintChannelSpan
    Fretless_setMidiHingBendSemis
 
 This function can be called at any time thereafter, generally when it is known that all fingers
 are up.  This can give us a silent reboot that can recover after an assert fail with no
 audible problems.
 */
void Fretless_boot(struct Fretless_context* ctxp)
{
    /**
     Reset everything except hints and external callbacks.
     This can be done to attempt recovery at a safe time 
     when an assertion has failed.  The main idea is that we try to have
     absolutely perfect code, but if something still goes wrong, then we
     try to go back to the initial state of the state machine when we know
     that all fingers are up.
     */
    for(int c=0; c<CHANNELMAX; c++)
    {
        ctxp->channels[c].lastBend = BENDCENTER;
        ctxp->channels[c].useCount = 0;
        ctxp->channels[c].currentFingerInChannel = NOBODY;
        ctxp->channels[c].lastAftertouch = 0;
        for(int n=0; n<NOTEMAX; n++)
        {
            ctxp->noteChannelDownCount[n][c] = 0;
            ctxp->noteChannelDownRawBalance[n][c] = 0;
        }
    }
    for(int f=0; f<FINGERMAX; f++)
    {
        Fretless_reset_FingerState(&ctxp->fingers[f]);
    }
    for(int p=0; p<POLYMAX; p++)
    {
        ctxp->polys[p].currentFingerInPolyGroup = NOBODY;
    }
    ctxp->fingersDownCount = 0;
    ctxp->lastAllocatedChannel = 0;
    
    //Ensure that channels are in some consistent state
    if(ctxp->channelSpan == 0)ctxp->fail("Fretless_state.channelSpan == 0\n");
    if(ctxp->channelBase < 0)ctxp->fail("%d: Fretless_state.channelBase < 0\n", ctxp->channelBase);
    if(ctxp->channelBase >= CHANNELMAX)ctxp->fail("Fretless_state.channelBase >= CHANNELMAX\n");
    if(ctxp->channelSpan + ctxp->channelBase >= CHANNELMAX)
    {
        ctxp->fail("Fretless_state.channelSpan:%d + Fretless_state.channelBase:%d >= CHANNELMAX\n",ctxp->channelSpan, ctxp->channelBase);
    }
    ctxp->ctxState=CTXSTATE_BOOTED;
    Fretless_setMidiHintChannelBendSemis(ctxp,ctxp->channelBendSemis);
}

static int Fretless_limitVal(int low,int val,int high)
{
    if(val < low)return low;
    if(val > high)return high;
    return val;
}

static void Fretless_fnoteToNoteBendPair(struct Fretless_context* ctxp, float fnote,int* notep,int* bendp)
{
    //Find the closest 12ET note
    *notep = (int)(fnote+0.5);
    //Compute the bend in terms of -1.0 to 1.0 range
    float floatBend = (fnote - *notep);
    *bendp = (BENDCENTER + floatBend*BENDCENTER/ctxp->channelBendSemis);
}

static void Fretless_fnoteBendFromExisting(struct Fretless_context* ctxp, float fnote,int* notep,int* bendp,
                                           struct Fretless_fingerState* fsPtr)
{
    //Use existing note
    int note = fsPtr->note;
    //Compute the bend in terms of -1.0 to 1.0 range from there
    float floatBend = (fnote - note);
    *bendp = (BENDCENTER + floatBend*BENDCENTER/ctxp->channelBendSemis);
    
    //If we exceeded the bend width, then generate a new note pair
    if(*bendp < 0 || *bendp >= 2*BENDCENTER)
    {
        Fretless_fnoteToNoteBendPair(ctxp, fnote, notep, bendp);
    }
    else
    {
        *notep = note;
    }
    //Caller can check to see if the note changed
}

static void Fretless_numTo7BitNums(int n,int* lop,int* hip)
{
    *lop = (    n  & 0x7f);
    *hip = ((n>>7) & 0x7f);
}

int Fretless_getChannelOccupancy(struct Fretless_context* ctxp, int channel)
{
    return ctxp->channels[channel].useCount;
}

float Fretless_getChannelBend(struct Fretless_context* ctxp, int channel)
{
    return (ctxp->channels[channel].lastBend - 8192) / 8192.0;
}

/**
 A non-exclusive alloc, that allocs in the least used channel that is in the span
 */
static int Fretless_allocChannel(struct Fretless_context* ctxp, int finger)
{
    //Look for an open exclusive slot on the channel that's the least in use.
    //Starting just after the last channel that was allocated to ensure maximum
    //release time for channel reuse
    //
    // 0 <= Fretless_state.channels[channel].useCount < inf
    for(int lowUsedCount=0; TRUE; lowUsedCount++)
    {
        for(int s=0; s<ctxp->channelSpan; s++)
        {
            //Always start just after the last allocated channel to maximize the time before channel is re-taken
            int span = ctxp->channelSpan;
            int base = ctxp->channelBase;
            int last = ctxp->lastAllocatedChannel;
            int candidate = last+1+s;
            int channel = (candidate-base)%span + base;
            //useCount should NEVER to below zero
            if(ctxp->channels[channel].useCount < 0)
            {
                ctxp->fail("Fretless_state.channels[channel].useCount < 0 on alloc\n");
                return 0;
            }
            if(ctxp->channels[channel].useCount == lowUsedCount)
            {
                ctxp->channels[channel].useCount++;
                //Insert this finger into the channel's linked list of fingers that use it,
                //and make it the current finger in the channel
                int currentFingerInChannel = ctxp->channels[channel].currentFingerInChannel;
                if(currentFingerInChannel != NOBODY)
                {
                    if(ctxp->fingers[currentFingerInChannel].nextFingerInChannel != NOBODY)
                    {
                        ctxp->fail("ctxp->fingers[currentFingerInChannel].nextFingerInChannel != NOBODY when allocating\n");
                    }
                    //point currentFingerInChannel and finger at each other
                    ctxp->fingers[currentFingerInChannel].nextFingerInChannel = finger;
                    ctxp->fingers[finger].prevFingerInChannel = currentFingerInChannel;
                }
                //Update the channel to make finger the leader
                ctxp->channels[channel].currentFingerInChannel = finger;
                //Ensure that the next alloc stays as far away from this channel as possible on next alloc
                ctxp->lastAllocatedChannel = channel;
                return channel;
            }
        }        
    }
    ctxp->fail("Fretless_allocChannel reached unreachable state\n");
    return 0;
}

static void Fretless_freeChannel(struct Fretless_context* ctxp, int finger)
{
    //Reduce the use count on this channel
    int channel = ctxp->fingers[finger].channel;
    ctxp->channels[channel].useCount--;
    if(ctxp->channels[channel].useCount < 0)
    {
        ctxp->fail("Fretless_state.channels[%d].useCount < 0 on free\n",channel);        
    }
    //Pull outselves out of the list
    int prevFinger = ctxp->fingers[finger].prevFingerInChannel;
    int nextFinger = ctxp->fingers[finger].nextFingerInChannel;
    int currentFinger = ctxp->channels[channel].currentFingerInChannel;
    
    //Point around us and select the leader (newest finger)
    if(prevFinger != NOBODY)
    {
        ctxp->fingers[prevFinger].nextFingerInChannel = nextFinger;
    }
    if(nextFinger != NOBODY)
    {
        ctxp->fingers[nextFinger].prevFingerInChannel = prevFinger;
    }    
    ctxp->fingers[finger].prevFingerInChannel = NOBODY;
    ctxp->fingers[finger].nextFingerInChannel = NOBODY;
    if(currentFinger == finger)
    {
        ctxp->channels[channel].currentFingerInChannel = prevFinger;
    }
}


void Fretless_noteTie(struct Fretless_context* ctxp,struct Fretless_fingerState* fsPtr)
{
    int lsb;
    int msb;
    Fretless_numTo7BitNums(1223,&lsb,&msb);
    int channel = fsPtr->channel;
    int note = fsPtr->note;
    //Coarse parm
    ctxp->midiPutch(0xB0 + channel);
    ctxp->midiPutch(0x63);
    ctxp->midiPutch(msb);
    //Fine parm
    ctxp->midiPutch(0xB0 + channel);
    ctxp->midiPutch(0x62);
    ctxp->midiPutch(lsb);
    //Val parm
    ctxp->midiPutch(0xB0 + channel);
    ctxp->midiPutch(0x06);
    ctxp->midiPutch(note);
    ///* I am told that the reset is bad for some synths
    /*
    ctxp->midiPutch(0xB0 + channel);
    ctxp->midiPutch(0x63);
    ctxp->midiPutch(0x7f);
    ctxp->midiPutch(0xB0 + channel);
    ctxp->midiPutch(0x62);
    ctxp->midiPutch(0x7f);
     */
     //*/
}

void Fretless_setCurrentBend(struct Fretless_context* ctxp, int finger)
{
    struct Fretless_fingerState* fsPtr = &ctxp->fingers[finger];
    if(ctxp->channels[fsPtr->channel].lastBend != fsPtr->bend && 
       ctxp->channels[fsPtr->channel].currentFingerInChannel == finger &&
       fsPtr->isOn &&
       ctxp->supressBends == FALSE)
    {
        ctxp->channels[fsPtr->channel].lastBend = fsPtr->bend;
        ctxp->midiPutch(MIDI_BEND + fsPtr->channel);
        int lo;
        int hi;
        Fretless_numTo7BitNums(fsPtr->bend, &lo, &hi);
        ctxp->midiPutch(lo);
        ctxp->midiPutch(hi);   
    }      
}

void Fretless_setCurrentAftertouch(struct Fretless_context* ctxp, int finger,float velocity)
{
    struct Fretless_fingerState* fsPtr = &ctxp->fingers[finger];
    //Update this finger's velocity
    fsPtr->velocity = Fretless_limitVal(1,velocity*127.0,127);
    if(ctxp->channels[fsPtr->channel].lastAftertouch != fsPtr->velocity && 
       ctxp->channels[fsPtr->channel].currentFingerInChannel == finger &&
       fsPtr->isOn &&
       ctxp->supressBends == FALSE)
    {
        ctxp->channels[fsPtr->channel].lastAftertouch = fsPtr->velocity;
        ctxp->midiPutch(0xD0 + fsPtr->channel);
        ctxp->midiPutch(fsPtr->velocity);
    }      
}

int Fretless_link(struct Fretless_context* ctxp,int finger)
{
    int polyGroup = ctxp->fingers[finger].polyGroup;
    int fingerToTurnOff = ctxp->polys[polyGroup].currentFingerInPolyGroup;
    if(fingerToTurnOff != NOBODY)
    {
        ctxp->fingers[fingerToTurnOff].isSupressed = TRUE;
        ctxp->fingers[fingerToTurnOff].nextFingerInPolyGroup = finger;
        ctxp->fingers[finger].prevFingerInPolyGroup = fingerToTurnOff;
    }
    ctxp->fingers[finger].polyGroup = polyGroup;
    ctxp->polys[polyGroup].currentFingerInPolyGroup = finger;
    return fingerToTurnOff;
}

/**
 Remove current finger from the linked list for this polyphony group.
 If we removed the current finger, then turn on the previous finger.
 */
int Fretless_unlink(struct Fretless_context* ctxp,int finger)
{
    int polyGroup = ctxp->fingers[finger].polyGroup;
    int currentFinger = ctxp->polys[polyGroup].currentFingerInPolyGroup;
    int prevFinger = ctxp->fingers[finger].prevFingerInPolyGroup;
    int nextFinger = ctxp->fingers[finger].nextFingerInPolyGroup;
    int fingerToTurnOn = NOBODY;
    
    //Remove ourselves from the list first
    if(prevFinger != NOBODY)
    {
        ctxp->fingers[prevFinger].nextFingerInPolyGroup = nextFinger;
    }
    if(nextFinger != NOBODY)
    {
        ctxp->fingers[nextFinger].prevFingerInPolyGroup = prevFinger;
    }    
    if(finger == currentFinger)
    {
        ctxp->polys[polyGroup].currentFingerInPolyGroup = prevFinger;
        fingerToTurnOn = prevFinger;
        if(fingerToTurnOn != NOBODY)
        {
            ctxp->fingers[fingerToTurnOn].isSupressed = FALSE;            
        }
    }
    
    ctxp->fingers[finger].prevFingerInPolyGroup = NOBODY;
    ctxp->fingers[finger].nextFingerInPolyGroup = NOBODY;
    ctxp->fingers[finger].polyGroup = NOBODY;
    return fingerToTurnOn;
}

//Must call this (per finger) before others are callable
void Fretless_beginDown(struct Fretless_context* ctxp, int finger)
{
    STATECHECK(ctxp)
    FINGERCHECK(ctxp,finger)

    struct Fretless_fingerState* fsPtr = &ctxp->fingers[finger];
    if(fsPtr->isOn == TRUE)
    {
        ctxp->fail("finger %d: Fretless_down && fsPtr->isOn == TRUE\n",finger);
    }
    fsPtr->isOn = TRUE;
    
    fsPtr->channel = Fretless_allocChannel(ctxp,finger);
}

//Must call this (per finger) before others are callable
void Fretless_endDown(struct Fretless_context* ctxp, int finger,float fnote,int polyGroup,float velocity,int legato)
{
    STATECHECK(ctxp)
    FINGERCHECK(ctxp,finger)
    POLYCHECK(ctxp,polyGroup)
    FNOTECHECK(ctxp,fnote)
    struct Fretless_fingerState* fsPtr = &ctxp->fingers[finger];
    if(fsPtr->isOn == FALSE)
    {
        ctxp->fail("finger %d: Fretless_down && fsPtr->isOn == FALSE\n",finger);
    }
    fsPtr->velocity = Fretless_limitVal(1,velocity*127,127); //Don't allow a send of zero here for balance purposes
    fsPtr->polyGroup = polyGroup;
    
    Fretless_fnoteToNoteBendPair(ctxp,fnote, &fsPtr->note, &fsPtr->bend);
    
    ctxp->fingersDownCount++;
    ctxp->noteChannelDownCount[fsPtr->note][fsPtr->channel]++;
    
    //Only send note off before on if there is more than one note residing here
    if(fsPtr->isSupressed == FALSE)
    {
        if(ctxp->noteChannelDownCount[fsPtr->note][fsPtr->channel]>1)
        {
            ctxp->midiPutch(MIDI_ON + fsPtr->channel);
            ctxp->midiPutch(fsPtr->note);
            ctxp->midiPutch(0);
            ctxp->noteChannelDownRawBalance[fsPtr->note][fsPtr->channel]--;            
        }        
    }
    
    //See if we just took over in our poly group
    int fingerTurningOff = Fretless_link(ctxp,finger);
    Fretless_setCurrentBend(ctxp, finger);
    
    if(finger != ctxp->channels[fsPtr->channel].currentFingerInChannel)
    {
        ctxp->fail("finger %d should be current in channel because it's note down\n",finger);        
    }
    if(fingerTurningOff != NOBODY)
    {    
        struct Fretless_fingerState* turningOffPtr = &ctxp->fingers[fingerTurningOff];        
        if(turningOffPtr->isOn == FALSE)
        {
            ctxp->fail("turningOffPtr->isOn should be on\n");
        }
        if(turningOffPtr->isSupressed == FALSE)
        {
            ctxp->fail("turningOffPtr->isSupressed should be supressed\n");
        }
        if(legato == 2)
        {
            Fretless_noteTie(ctxp,turningOffPtr);            
        }
        ctxp->midiPutch(MIDI_ON + turningOffPtr->channel);
        ctxp->midiPutch(turningOffPtr->note);
        ctxp->midiPutch(0);
        ctxp->noteChannelDownRawBalance[turningOffPtr->note][turningOffPtr->channel]--;
    }
    ctxp->midiPutch(MIDI_ON + fsPtr->channel);
    ctxp->midiPutch(fsPtr->note);
    ctxp->midiPutch(fsPtr->velocity);
    ctxp->noteChannelDownRawBalance[fsPtr->note][fsPtr->channel]++;
    if( ctxp->noteChannelDownRawBalance[fsPtr->note][fsPtr->channel] > 1 )
    {
        ctxp->logger("we sent out a doubled note on down ch%d n%d\n",fsPtr->channel,fsPtr->note);            
    }
}



//Free up the finger
void Fretless_up(struct Fretless_context* ctxp, int finger,int legato)
{
    FINGERCHECK(ctxp,finger)
    
    struct Fretless_fingerState* fsPtr = &ctxp->fingers[finger];
    if(fsPtr->isOn == FALSE)
    {
        ctxp->fail("finger %d: Fretless_up && fsPtr->isOn == FALSE\n",finger);
    }
    
    int oldVelocity = fsPtr->velocity;
    int fingerWasSupressed = fsPtr->isSupressed;
    int fingerToTurnOn = Fretless_unlink(ctxp, finger);
            
    //Temporarily disable the note if we are overbooking channels
    ctxp->noteChannelDownCount[fsPtr->note][fsPtr->channel]--;
    
    if(fingerWasSupressed==FALSE)
    {
        if(ctxp->noteChannelDownCount[fsPtr->note][fsPtr->channel] == 0)
        {
            if(fingerToTurnOn != NOBODY)
            {
                if(legato > 0)
                {
                    Fretless_noteTie(ctxp, fsPtr);                    
                }
            }
            ctxp->midiPutch(MIDI_ON + fsPtr->channel);
            ctxp->midiPutch(fsPtr->note);
            ctxp->midiPutch(0);
            ctxp->noteChannelDownRawBalance[fsPtr->note][fsPtr->channel]--;            
        }        
    }    
    
    //If we uncovered a note by picking up current note on poly group...
    if(fingerToTurnOn != NOBODY)
    {
        struct Fretless_fingerState* turningOnPtr = &ctxp->fingers[fingerToTurnOn];        
        if(turningOnPtr->isOn == FALSE)
        {
            ctxp->fail("turningOnPtr->isOn should be on\n");
        }
        if(turningOnPtr->isSupressed == TRUE)
        {
            ctxp->fail("turningOffPtr->isSupressed should not be supressed\n");
        }
        //Set the bend wrong to force a re-send (note ups dont happen often enough that its a problem)
        ctxp->channels[turningOnPtr->channel].lastBend = -1;
        Fretless_setCurrentBend(ctxp,fingerToTurnOn);
        //Adopt the velocity of the note that uncovers us
        turningOnPtr->velocity = oldVelocity;
        ctxp->midiPutch(MIDI_ON + turningOnPtr->channel);
        ctxp->midiPutch(turningOnPtr->note);
        ctxp->midiPutch(turningOnPtr->velocity);
        ctxp->noteChannelDownRawBalance[turningOnPtr->note][turningOnPtr->channel]++;
        if( ctxp->noteChannelDownRawBalance[fsPtr->note][fsPtr->channel] > 1 )
        {
            ctxp->logger("we sent out a doubled note on up ch%d n%d\n",fsPtr->channel,fsPtr->note);            
        }
    }
    
    if(ctxp->noteChannelDownCount[fsPtr->note][fsPtr->channel]<0)
    {
        ctxp->fail("Fretless_state.noteChannelDownCount[%d][%d]== %d\n",
                   fsPtr->note,fsPtr->channel,ctxp->noteChannelDownCount[fsPtr->note][fsPtr->channel]);
    }
    
    ctxp->fingersDownCount--;
    if(ctxp->fingersDownCount<0)
    {
        ctxp->fail("Fretless_state.fingersDownCount == %d\n",ctxp->fingersDownCount);
    }
    
    fsPtr->isOn = FALSE;
    Fretless_freeChannel(ctxp,finger);
    Fretless_reset_FingerState(fsPtr);
    
    
    if(ctxp->fingersDownCount <= 0)
    {
        Fretless_selfTest(ctxp);
    }
}

//Callable for down or move, before flush - key should be a valid CC
void Fretless_express(struct Fretless_context* ctxp, int finger,int key,float val)
{
    FINGERCHECK(ctxp,finger)  
    struct Fretless_fingerState* fsPtr = &ctxp->fingers[finger];
    if(fsPtr->isOn == FALSE)
    {
        ctxp->fail("finger %d: Fretless_express && fsPtr->isOn == FALSE\n",finger);
    }    
    
    ctxp->midiPutch(0xB0+ fsPtr->channel);
    ctxp->midiPutch(key % 127);
    ctxp->midiPutch(((int)(val*127)) % 127);   
}

float Fretless_move(struct Fretless_context* ctxp, int finger,float fnote,float velocity,int polyGroup)
{
    FINGERCHECK(ctxp,finger)
    FNOTECHECK(ctxp,fnote)
    
    //Determine the bend that's wanted
    struct Fretless_fingerState* fsPtr = &ctxp->fingers[finger];
    if(fsPtr->isOn == FALSE)
    {
        ctxp->fail("finger %d: Fretless_move && fsPtr->isOn == FALSE\n",finger);
    }
    int newNote;
    int newBend;
    Fretless_fnoteBendFromExisting(ctxp,fnote, &newNote, &newBend,fsPtr);
    //If it's just a bend of the current note, then do that
    int existingPolyGroup = fsPtr->polyGroup;
    if(0 <= polyGroup && polyGroup < FINGERMAX)
    {
        fsPtr->visitingPolyGroup = polyGroup;        
    }
    if(newNote == fsPtr->note)
    {
        fsPtr->bend = newBend;
        Fretless_setCurrentAftertouch(ctxp,finger,velocity);
        Fretless_setCurrentBend(ctxp,finger);            
    }    
    else
    {
        Fretless_noteTie(ctxp,fsPtr);            
        Fretless_up(ctxp,finger,TRUE);
        Fretless_beginDown(ctxp,finger);
        Fretless_endDown(ctxp,finger,fnote,existingPolyGroup,velocity,TRUE);
    }
    return fnote;
}

//sequence finish.  we can send it now
void Fretless_flush(struct Fretless_context* ctxp)
{
    ctxp->midiFlush();
}

//Look for consistency.  We have checks just for when all fingers are known up.
//We could run this on idle to detect problems.
void Fretless_selfTest(struct Fretless_context* ctxp)
{
    int passed = TRUE;
    if(ctxp->fingersDownCount == 0)
    {
        for(int c=0; c<CHANNELMAX; c++)
        {
            int useCount = ctxp->channels[c].useCount;
            if(useCount != 0)
            {
                ctxp->fail("%d: Fretless_selfTest() Fretless_state.fingersDownCount==0 && useCount != 0\n", useCount);
                passed = FALSE;
            }
            for(int n=0; n<NOTEMAX; n++)
            {
                if(ctxp->noteChannelDownCount[n][c] != 0)
                {
                    ctxp->fail("Fretless_state.noteChannelDownCount[0x%d][0x%d] == %d\n",n,c, ctxp->noteChannelDownCount[n][c]);
                    passed = FALSE;
                }
                
                if(ctxp->noteChannelDownRawBalance[n][c] != 0)
                {
                    if(ctxp->noteChannelDownRawBalance[n][c] < 0)
                    {
                        int foundVal = ctxp->noteChannelDownRawBalance[n][c];
                        ctxp->noteChannelDownRawBalance[n][c] = 0;    
                        ctxp->logger("ctxp->noteChannelDownRawBalance[%2x][%2x] == %d\n",n,c,foundVal);
                    }
                    else
                    {
                        ctxp->fail("Fretless_state.noteChannelDownRawBalance[0x%2x][0x%2x] == %d\n",n,c, ctxp->noteChannelDownRawBalance[n][c]);
                        passed = FALSE;                        
                    }
                }
                 
            }
            if(ctxp->channels[c].currentFingerInChannel != NOBODY)
            {
                ctxp->fail("ctxp->channels[0x%2x].currentFingerInChannel != NOBODY\n",c);
                passed = FALSE;
            }
        }
        for(int p=0; p<POLYMAX; p++)
        {
            if(ctxp->polys[p].currentFingerInPolyGroup != NOBODY)
            {
                ctxp->fail("poly group useCount is wrong\n");
                passed = FALSE;
            }
        }
        for(int f=0; f<FINGERMAX; f++)
        {
            if(ctxp->fingers[f].isOn)
            {
                ctxp->fail("Fretless_selfTest() Fretless_state.fingers[%d].isOn\n",f);
                passed = FALSE;
            }
            if(ctxp->fingers[f].nextFingerInChannel != NOBODY)
            {
                ctxp->fail("ctxp->fingers[%d].nextFingerInChannel != NOBODY\n",f);
                passed = FALSE;
            }
            if(ctxp->fingers[f].prevFingerInChannel != NOBODY)
            {
                ctxp->fail("ctxp->fingers[%d].prevFingerInChannel != NOBODY\n",f);
                passed = FALSE;
            }
        }
    }
    if(ctxp->fingersDownCount < 0)
    {
        ctxp->fail("less than zero fingers count!\n");
        passed = FALSE;
    }
    //Let the owner know that we passed self tests
    if(passed)
    {
        ctxp->passed();
    }
    else
    {
        
        //Force a recovery and quiet reboot
        for(int n=0; n<NOTEMAX; n++)
        {
            //Some stuff doesn't respond to all notes off.  Use brute force!
            for(int c=0; c<CHANNELMAX; c++)
            {
                //Turn it off!
                ctxp->midiPutch(MIDI_ON+c);
                ctxp->midiPutch(n);
                ctxp->midiPutch(0);                                    
            }
            Fretless_flush(ctxp);
        }
        //recover
        Fretless_boot(ctxp);
    }
}




