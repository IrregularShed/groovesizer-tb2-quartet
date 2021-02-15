# Groovesizer TB2 - Quartet Firmware

### What is this?

This is the [Quartet Firmware](https://groovesizer.com/tb2-resources/) for the [Groovesizer TB2](https://groovesizer.com/tb2/) project, rejigged so that it can be compiled using [PlatformIO](https://platformio.org) with something like VS Code instead of having to use the standard [Arduino](https://arduino.cc) IDE. Why? Because the Arduino IDE is horrible, dated and lacking features you'd expect in 2021. PlatformIO has some quirks but it's a better option with VS Code for big projects (and this is quite sizable). I tried the Arduino extension for VS Code but I found it far less mature.

### What needed doing?

The firmware as provided is multiple `.ino` files, which is fine in the Arduino IDE (it concatenates them during the compilation process) but it brings problems with 'proper' compilers that don't do that. In Arduino you can drop functions and variable declarations in any other `.ino` file without having to import them in any way; it's not as simple as renaming them to `.cpp `and having it all just work (not by a long stretch). Arduino also lets you drop your functions into the `.ino` file in any order you like, instead of declaring them in advance as bona fide C++ needs you to do.

### What changes have been made?

The biggest change is alluded to above - the 18 individual `.ino` files have been merged into one enormous `.cpp` file (over 6000 lines). A header file has been created for all the functions and the masses of global variables that the firmware uses (see the `/include` directory, it's a `.h` instead of a `.hpp` purely to match the other `#include`s). The libraries that the firmware requires are included in the `/lib` directory; one of the `#include` lines in the SdFat library needed modifying for the compiler to work, but otherwise they're stock as provided on the Groovesizer website.

I've not made any other changes (yet), except to bump the version number because this is quite a big undertaking. I want to have different display options though, because these character LCDs are enormous, and I obviously want to split the enormous single `.cpp` file into more manageable chunks, but it's been so long since I really did anything in C++ that I can't remember what to do or where to start.

### Why are you doing this Steve?

Because it's a great project, makes great sounds, and I want to do more with it. But not in the Arduino IDE, which (as established) I hate. Both hardware and software are open source, so I've made my own boards and here's my firmware.

Have a look at this demo on the YouTubes...

[![Groovesizer TB2](http://img.youtube.com/vi/c9mzL6aF-kU/0.jpg)](http://www.youtube.com/watch?v=c9mzL6aF-kU 'Groovesizer TB2')

### What next?

In the short term I need to get a license file in here and it would be handy to redraw the schematics so that anyone else can have a go. As I get used to this project I'd like to have support for CV inputs for the filters and things like that, because ultimately getting this in a rack would be sweet as flip.
