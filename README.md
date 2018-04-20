# vdex2dex

A tool that uses the ART libraries to dump a DEX file from a VDEX (verified DEX?) file.

Very loosely based on dex2oat. (More like I took its source file and emptied it.)

## Usage

1. Insert this directory into `/art` in your copy of AOSP.
2. Insert `"vdex2dex"` into `subdirs` in `/art/Android.bp`.
3. `mmma art/vdex2dex`
4. `vdex2dex app.vdex classes.dex`
