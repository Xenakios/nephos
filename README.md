![Diagram](./Assets/nephos_screenshot_01.png)

# Nephos - opinionated granular synthesis

Grains from pure synthesis, spatialized into Ambisonics.

By now, granular synthesis has largely come to mean granulating audio files, buffers,
or samples, but Nephos takes a different approach and generates its grains from classic
waveforms (sine, semisine, triangle, saw, square) as well as 2-operator FM synthesis and
noise. While these might not be the most interesting sound sources on their own, much
more variety can be achieved through additional parameters, grain-internal envelopes,
and insert filters/effects per grain. This also harks back to the origins of granular
synthesis in the 1950s, when Iannis Xenakis used sine wave generators to produce grains
for the tape composition *Analogique B*.

Spatialization of the grains is an important aspect of Nephos. Instead of panning each
grain directly into a stereo, quad, 5.1 surround, or other speaker arrangement, it uses
Ambisonics — up to 7th order — for spatial placement. The user then needs to decode that
Ambisonics signal as desired. For simple stereo use, a built-in decoder (essentially M/S
decoding) is provided. However, it is recommended to use Nephos properly with Ambisonics
in a DAW that supports the required channel counts to pass the Ambisonics signal — for
example, 16 channels for 3rd order Ambisonics. The [IEM plugin suite](https://plugins.iem.at/)
is recommended for visualization, post-processing, and decoding of Nephos's output.

Since the synthesis architecture in Nephos doesn't have a concept of triggered voices, no straightforward way to play Nephos with MIDI keyboard/notes is provided. It can receive MIDI note messages and the key, velocity and polyphonic aftertouch can be used as modulation sources, though. It will be quite challenging to play standard music that way and this is deliberate, again looking back to the beginnings of experimental music. 
Nephos should be considered more like a modular synthesizer module[^1] and not a traditional virtual synth. In the future, ways to use Nephos more easily for standard music may be developed, but this is not a priority at the moment. 

[^1]: This may raise the question why Nephos wasn't developed for example as a VCV Rack module, then? The reason would be that VCV Rack and its ecosystem does not currently (2026) adequately support the considerable amount of audio channels needed for the higher Ambisonic orders. If VCV Rack increases the channel count per cable to 64 channels from the current 16 channels and makes their VCV Host module for hosting 3rd party plugins compatible with high channel counts, a VCV Rack module of Nephos may be developed in the future.
