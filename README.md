![Diagram](./Assets/nephos_screenshot_01.png)

# Nephos - granular synthesizer

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