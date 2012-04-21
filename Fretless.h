//
//  Fretless.h
//  AlephOne
//
//  Created by Robert Fielding on 10/14/11.
//
// This should remain a *pure* C library with no references to external libraries

/*
 * There is no internal global state, so this entire library is re-entrant.
 * You may configure multiple instances of this library differently and have them
 * send different MIDI streams.
 *
 * This library is just a buffer generator for the MIDI protocol.  It hides all
 * of the problems with MIDI associated with pitch.  It provides a pitch oriented
 * description of the gestures for the client.  The client isn't really dependent upon
 * knowing anything about MIDI, as it's just getting buffers made.
 */
struct Fretless_context;

/*
 * Get a context for the Fretless API.
 * This is the first thing that we can call.
 * 
 * Inject dependencies here, such as the allocation strategy (ie: malloc),
 * logging, failures, and positive acknowlegement that everything seems ok.
 */
struct Fretless_context* Fretless_init(
                                       void (*midiPutch)(char),void (*midiFlush)(), 
                                       void* (*fretlessAlloc)(unsigned long), 
                                       void (*fretlessFree)(void*),
                                       int (*fail)(const char*,...), 
                                       void (*passed)(),
                                       int (*logger)(const char*,...)
                                       );

void Fretless_free(struct Fretless_context* ctxp);

/*
 * When we channel cycle, this is the lowest channel in that adjacent span of channels
 */
void Fretless_setMidiHintChannelBase(struct Fretless_context* ctxp, int base);
int Fretless_getMidiHintChannelBase(struct Fretless_context* ctxp);

/*
 * This tells us how many channels to cycle across
 */
void Fretless_setMidiHintChannelSpan(struct Fretless_context* ctxp, int span);
int Fretless_getMidiHintChannelSpan(struct Fretless_context* ctxp);

/*
 * This is the number of semitones that a maxmimized bend will span
 */
void Fretless_setMidiHintChannelBendSemis(struct Fretless_context* ctxp, int semitones);
int Fretless_getMidiHintChannelBendSemis(struct Fretless_context* ctxp);

/*
 * Use this to note that we would like to not send out bends to MIDI
 */
void Fretless_setMidiHintSupressBends(struct Fretless_context* ctxp, int supressBends);

/*
 * Once MIDI is configured, invoke this to get ready to call other functions such as:
 *   up,down,move,express,flush
 */
void Fretless_boot(struct Fretless_context* ctxp);

/*
 * A finger going down, with a floating point value for the MIDI note.
 * Polyphony groups are kind of like channels, where polyphony and legato happens.
 * If each note goes down into its own polyphony group, then there will be no legato
 * or unusual limits to polyphony.  If all notes are in same polyphony group, then it's like solo
 * mode on a keyboard.  If some notes are in different polyphony groups while there are notes in same
 * group, then there will be a combination of chording and legato available.
 *
 * When this note goes on, it will supress other notes in the same polyphony group if they exist.
 *
 * It is split into begin and end so that express calls can be sandwiched in the middle.
 * This ensures that the parameter is correct before the note begins.
 *
 *  legato=0 means no legato
 *  legato=1 means legato on up
 *  legato=2 means legato on up/down
 *
 *  after beginDown, you can put in some number of calls to express, but must call
 *  endDown after that.
 */
void Fretless_beginDown(struct Fretless_context* ctxp, int finger);
void Fretless_endDown(struct Fretless_context* ctxp, int finger,float fnote,int polyGroup,float velocity,int legato);
/*
 * Invoke this from the controller to send expression.  It sends nothing right now, but should
 * send MIDI CCs at some point.
 *
 * This can be invoked between beginDown and endDown
 */
void Fretless_express(struct Fretless_context* ctxp, int finger,int key,float val);

/*
 * Move a finger around, and optionally state if it happened to move into a new logical polyphony
 * group.
 */
float Fretless_move(struct Fretless_context* ctxp, int finger,float fnote,float velocity,int polyGroup);

/*
 * The finger came up.  It will turn this note off, but it will also trigger the lead note
 * in the same polyphony group if it exists.
 */
void Fretless_up(struct Fretless_context* ctxp, int finger,int legato);

/*
 * Mark a boundary for this gesture.  Tell MIDI rendering to mark this point as a boundary.
 */
void Fretless_flush(struct Fretless_context* ctxp);

/*
 * Get some detail on how many notes live in each channel.  This is one of the few places where
 * the fact that it's MIDI underneath are being allowed to leak through.  But I need this info
 * in the user interface rendition.
 */
int   Fretless_getChannelOccupancy(struct Fretless_context* ctxp, int channel);
float Fretless_getChannelVolume(struct Fretless_context* ctxp, int channel);

/*
 * Get detail on the bend away from the 12ET note
 */
float Fretless_getChannelBend(struct Fretless_context* ctxp, int channel);
