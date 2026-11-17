# ZIBENS 0.4 (Digitakt MKI, OS 1.54)

*Zibens* is Latvian for lightning. A drum-synth machine laid out like a four-voice drum synth:
pick a voice, one of its three algorithms, shape it with two parameters, then decay, drive, a
gritty delay and a compressor. Pick it with **FUNC + SRC**. The Digitakt's filter, amp, LFO and FX
follow, as on a sample.

## Page 1: VOICE

| Knob | |
|---|---|
| TUNE | pitch |
| VOIC | KICK, LOW, SNARE or HATS. Its picture is the drum |
| ALGO | one of the voice's three algorithms (below) |
| PRM1, PRM2 | the algorithm's two parameters. The pop-up names them |
| DEC | the voice's own decay, 8 ms to 4 s. Its picture is the fall |
| DRIV | drive, from clean to crushed |
| LEV | level |

| VOIC | ALGO | PRM1 | PRM2 |
|---|---|---|---|
| KICK | 808: a sine | BODY: rounder, harder | SUB: an octave below |
| | FM | INDX: FM amount, falling with the pitch envelope | RATIO: 1-4 |
| | PNCH: a pulse through a low-pass | WIDTH | TONE |
| LOW | TOM: two sines | DETUN | NOISE at the hit |
| | RES: a burst of noise rings a resonator | RESO | BURST length |
| | ZAP: a saw, its low-pass sweeping | RESO | SWEEP |
| SNARE | SNR: two heads and noise | BODY vs noise | TONE of the noise |
| | CLAP: three claps, then a tail | SPRD between claps | TONE |
| | METL: two squares ring-modulated (rims, cowbells) | RATIO | NOISE |
| HATS | HAT: the 808's six squares | METAL vs noise | HP |
| | NOIS: noise through a resonant band | RESO | COLR: band to bright |
| | BIT: chip noise | RATE | LOOP: long noise, or the short metallic loop |

## Page 2: FX (press SRC again)

| Knob | |
|---|---|
| PENV | pitch envelope, up to +48 semitones at the hit |
| PDEC | how fast the pitch falls, 2-400 ms |
| SEND | how much goes to the delay |
| TIME | delay time, 10-340 ms |
| FDBK | delay feedback |
| COMP | compressor: squeezes the hit and brings up the body |
| DEC | the same DEC as on page 1 |
| VIEW | the envelopes across DEC and VIEW: level (line) and pitch (dots). VIEW zooms: 100 ms, 400 ms, 1.5 s, 4 s |

The delay works like a bucket-brigade one: darker on every repeat, a slow wobble, 8-bit grit. Each
ZIBENS track has its own. Its tail lasts only as long as the Digitakt's AMP envelope lets it, so give
the AMP a long decay (or HOLD) when you want the echoes to ring out.

## How much it can play

The voices are worked out at 24 kHz and filled out to 48 kHz (snares and hats held sample by sample,
so their noise stays bright), and a track costs nothing once its sound and its echoes have died
away. Long hat decays on many tracks are the heaviest case.

## Install

Load `zibens-0.4.elemod` in Elekloader with **core 4.4** and **machine-pages**, build your OS and
flash it. No `.syx` is shared here: it would contain Elektron's firmware.

## Notes

Needs **core 4.4** and **machine-pages**. Machine id 34. Page 2 uses sound slots 46-52, like every
machine-pages machine with a second page, so on a ZIBENS track don't use FX Locks or Deja Vu's RND
destinations. With DIGIWAVE and all the flexmods loaded, mod memory is at 251 of 256 KB.

Licence: GPL-2.0-or-later. By Cere, made with Claude. Not affiliated with Erica Synths.
