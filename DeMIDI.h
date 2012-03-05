/*
   Parse MIDI messages into raw pitch and expression.
   This is how an internal engine gets built.
   Its main interface with the outside world is putch/flush.
 */

void DeMIDI_start(void (*rawEngine)(int midiChannel,int doNoteAttack,float pitch,float volVal,int midiExprParm,int midiExpr));
void DeMIDI_stop();

void DeMIDI_putch(char c);
void DeMIDI_flush();