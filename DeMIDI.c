//
//  DeMIDI.c
//  AlephOne
//
//  Created by Robert Fielding on 2/1/12.
//
/**
    We are only handling one note per channel, since our instrument is designed
    to spread across 16 channels anyway.  This dramatically simplifies the engine.
 */

#include "DeMIDI.h"
#include <stdio.h>
#include "FretlessCommon.h"



static void (*rawEngine)(int midiChannel,int doNoteAttack,float pitch,float volVal,int midiExprParm,int midiExpr);


void DeMIDI_start(void (*rawEngineArg)(int midiChannel,int doNoteAttack,float pitch,float volVal,int midiExprParm,int midiExpr))
{
    rawEngine = rawEngineArg;
    //Start the sound engine
}

void DeMIDI_stop()
{
    //Stop the sound engine
}

#define S_EXPECT_STATUS 0
#define S_ON_BYTE_NOTE 1
#define S_ON_BYTE_VOL 2
#define S_OFF_BYTE_NOTE 3
#define S_OFF_BYTE_VOL 4
#define S_BEND_LO 5
#define S_BEND_HI 6
#define S_RPN_LO 7
#define S_RPN_HI 8
#define S_NRPN_LO_KEY 9
#define S_NRPN_HI_KEY 10
#define S_RPN_VAL 11
#define S_RPN_LO_KEY 12
#define S_RPN_HI_KEY 13
#define S_CH_PRESS 14
#define S_RPN_11 15

int expectState=S_EXPECT_STATUS;

int midiStatus = 0;
int midiChannel = 0;
int expectDataBytes = 0;
int midiNote[FINGERMAX];
int midiVol[FINGERMAX];
int doNoteAttack = 0;
int midiExprParm = 0;
int midiExpr = 0;
int midiPitchBendSemis = 2;
int midiBend[FINGERMAX];
int nrpnKeyLo;
int nrpnKeyHi;
int rpnKeyLo;
int rpnKeyHi;
int rpnVal;
int expectNoteTie = 0;
//Handle ambiguity if 6 and 38 in nrpn setting here.
int isRegistered = 0;

float computePitch(int channel)
{
    return 1.0 * midiNote[channel] + (midiPitchBendSemis*(midiBend[channel] - 8192))/8192.0;    
}

float computeVol(int channel)
{
    return midiVol[channel] / 127.0;                
}

/**
 Just do the decode as an FSM
 The nrpn/rpn stuff is just nuts...
 */
void DeMIDI_putch(char c)
{    
    if(expectState == S_EXPECT_STATUS)
    {
        for(int i=0; i<FINGERMAX; i++)
        {
            midiBend[i] = 8192;
        }
    }
    //Handle status bytes to get overall state
    if( c & 0x80 )
    {
        midiStatus = (c & 0x00F0) >> 4;
        midiChannel = (c & 0x000F);
        switch( midiStatus )
        {
            case 0x08:
                expectState = S_OFF_BYTE_NOTE;
                return;
            case 0x09:
                expectState = S_ON_BYTE_NOTE;
                return;
            case 0x0B:
                expectState = S_RPN_LO;
                return;
            case 0x0D:
                expectState = S_CH_PRESS;
                return;
            case 0x0E:
                expectState = S_BEND_LO;
                return;
            default:
                printf("we don't recognize status byte %d yet.\n",midiStatus);
                return;
        }
    }
    else
    {
        //Handle data bytes
        switch( expectState)
        {
            case S_ON_BYTE_NOTE:
                midiNote[midiChannel] = (int)(c & 0x7F);
                expectState = S_ON_BYTE_VOL;
                return;
            case S_ON_BYTE_VOL:
                midiVol[midiChannel] = (int)(c & 0x7F);
                expectState = S_ON_BYTE_NOTE;
                rawEngine(midiChannel,doNoteAttack,computePitch(midiChannel),computeVol(midiChannel),midiExprParm,midiExpr);
                return;
                
            case S_OFF_BYTE_NOTE:
                midiNote[midiChannel] = (int)(c & 0x7F);
                expectState = S_OFF_BYTE_NOTE;
                return;
            case S_OFF_BYTE_VOL:
                midiVol[midiChannel] = 0;
                expectState = S_OFF_BYTE_NOTE;
                rawEngine(midiChannel,doNoteAttack,computePitch(midiChannel),0,midiExprParm,midiExpr);
                return;
                
            case S_BEND_LO:
                midiBend[midiChannel] = (int)(c & 0x7F);
                expectState = S_BEND_HI;
                return;
            case S_BEND_HI:
                midiBend[midiChannel] = (((int)(c & 0x7F))<<7) + midiBend[midiChannel];
                expectState = S_BEND_LO;
                rawEngine(midiChannel,doNoteAttack,computePitch(midiChannel),computeVol(midiChannel),midiExprParm,midiExpr);
                return;
            
            case S_RPN_LO:
                {
                    switch((int)(c & 0x7F))
                    {
                        case 0x63:
                            expectState = S_NRPN_LO_KEY;
                            return;
                        case 0x62:
                            expectState = S_NRPN_HI_KEY;
                            return;
                        case 101:
                            expectState = S_RPN_LO_KEY;
                            return;
                        case 100:
                            expectState = S_RPN_HI_KEY;
                            return;
                        case 0x06:
                            expectState = S_RPN_VAL;
                            return;
                        case 11:
                            expectState = S_RPN_11;
                        return;
                    }                
                }
                return;
            case S_NRPN_LO_KEY:
                isRegistered = 0;
                nrpnKeyLo = (int)(c & 0x7F);
                return;
            case S_NRPN_HI_KEY:
                isRegistered = 0;
                nrpnKeyHi = (int)(c & 0x7F);
                return;
            case S_RPN_VAL:
                rpnVal = (int)(c & 0x7F);
                if(isRegistered && rpnKeyLo == 0 && rpnKeyHi == 0)
                {
                    midiPitchBendSemis = rpnVal;
                }
                else
                if(isRegistered==0 && nrpnKeyLo == 9 && nrpnKeyHi == 71)
                {
                    //Next on/off pair should be tied together.
                    rawEngine(midiChannel,1,0,0,0,0);
                }
                return;
                
            case S_CH_PRESS:
                if(midiVol[midiChannel])
                {
                    midiVol[midiChannel] = (int)(c & 0x7F);
                    rawEngine(midiChannel,0,computePitch(midiChannel),computeVol(midiChannel),midiExprParm,midiExpr);                    
                }
                return;
                
            case S_RPN_LO_KEY:
                isRegistered = 1;
                rpnKeyLo = (int)(c & 0x7F);
                return;
            case S_RPN_HI_KEY:
                isRegistered = 1;
                rpnKeyHi = (int)(c & 0x7F);
                return;
            case S_RPN_11:
                midiExprParm = 11;
                midiExpr = (int)(c & 0x7F);
                return;
                
                
            case S_EXPECT_STATUS:
                printf("illegal state.\n we didn't start with status byte!\n");
                return;
            default:
                printf("skipping unrecognized data bytes in status %d\n", midiStatus);
                //Skip data bytes that we were not expecting
                return;
                
        }
    }
}


void DeMIDI_flush()
{
    //We don't do anything with data boundaries right now
}

