crn_decomp.h / crnlib.h - Crunch (CRN) to DXTn transcoder, from the crunch project
by Rich Geldreich (BinomialLLC/crunch). Public domain - see the header comment.

Only the *decompression* side is here; nothing from the compressor is used. These two
headers are what lets MetroEX read the .512c / .1024c / .2048c textures that the Metro
2033 Redux and Last Light Redux archives are almost entirely made of - those files are
Crunch streams (magic 0x4878), and without a transcoder there is no way to get pixels
out of them.

Source:
  https://raw.githubusercontent.com/BinomialLLC/crunch/master/inc/crn_decomp.h
  https://raw.githubusercontent.com/BinomialLLC/crunch/master/inc/crnlib.h

Retrieved 2026-08-29, sha256:
  crn_decomp.h  a55228061194e14cace14ffe1f892c2c2e99529a0c1b6475ff642957fb4c64ad
  crnlib.h      e745c166c9c42576279d26246bb0ff0cbc69db34109bdc9f4635d0b4ce50e3be

The implementation lives in the header, so exactly one translation unit
(src/crn_transcode.cpp) includes it without CRND_HEADER_FILE_ONLY.
