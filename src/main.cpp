/***********************************************************************************************************
***   GROOVESIZER TB2 Quartet - 4 Voice Paraphonic Synth and Step Sequencer
***   for the Arduino DUE with TB2 shield http://groovesizer.com
***   August 2014
***   by MoShang (Jean Marais) - moshang@groovesizer.com
***   Adapted from tutorials by DuaneB at http://rcarduino.blogspot.com
***   And the Groovuino library by Gaétan Ro at http://groovuino.blogspot.com
***
***   Mashed the fudge up by Steve for use with PlatformIO, Jan/Feb 2021
***
***   Licensed under a Creative Commons Attribution-ShareAlike 3.0
***   AKWF Waveforms by Adventure Kid is licensed under a Creative Commons Attribution 3.0 Unported License.
***   Based on a work at http://www.adventurekid.se
***   Tillstånd utöver denna licens kan vara tillgängligt från http://www.adventurekid.se.
************************************************************************************************************/

#include <Arduino.h>
#include <DueTimer.h>
#include <MIDI.h>
#include <TB2_LCD.h>
#include <SdFat.h>
#include <main.h>

// SETUP.ino
void setup()
{
  // DEBUG
  //Serial.begin(9600);

  // *** LCD ***
  // set up the LCD's number of columns and rows:
  lcd.begin(16, 2);

  // create custom characters
  lcd.createChar(2, Tee1);
  lcd.createChar(3, Tee2);
  lcd.createChar(4, Bee1);
  lcd.createChar(5, Bee2);
  lcd.createChar(6, Two1);
  lcd.createChar(7, Two2);

  lcd.setCursor(4, 0);
  lcd.print("Groovesizer");
  lcd.setCursor(4, 1);
  lcd.print("Ver.");
  lcd.setCursor(8, 1);
  lcd.print(versionNumber);
  showTB2(0);

  // *** BUTTONS ***
  // Make input & enable pull-up resistors on switch pins
  for (byte i = 0; i < NUMBUTTONS; i++)
    pinMode(buttons[i], INPUT_PULLUP);

  assignIncrementButtons(&menuChoice, 0, 6, 1);

  // *** POTS ***
  getPots();
  lockPot(5);

  // *** LED ***
  pinMode(LED, OUTPUT);
  digitalWrite(LED, LOW); // turn off the LED

  // *** SYNTH ***
  ulInput[0] = 512;

  createNoteTable(SAMPLE_RATE);
  createSineTable();
  createSquareTable(pulseWidth);
  createSawTable();
  createTriangleTable();
  clearUserTables();

  for (byte i = 0; i < 4; i++)
    wavePointer[i] = &nTriangleTable[0];
  for (byte i = 0; i < 4; i++)
    wavePointer[i + 4] = &nTriangleTable[0];

  // this is a cheat - enable the DAC
  analogWrite(DAC0, 0);
  analogWrite(DAC1, 0);

  // *** TIMERS ***
  Timer3.attachInterrupt(audioHandler).setFrequency(44100).start();            // start the audio interrupt at 44.1kHz
  Timer4.attachInterrupt(lfoHandler).setFrequency(22000).start();              // start the LFO handler
  Timer5.attachInterrupt(clockHandler).setPeriod(60000000 / bpm / 96).start(); // start the 96ppq internal clock (in microseconds)

  // *** FILTER ***
  setFilterCutoff(255);
  setFilterResonance(210);
  setFilterType(filterType); // 0 = LP, 1 = BP, 2 = HP

  // *** MIDI ***
  midiA.setHandleNoteOn(HandleNoteOn); // these callback functions are defined in MIDI
  midiA.setHandleNoteOff(HandleNoteOff);
  //midiA.setHandleControlChange(HandleCC);
  midiA.setHandleClock(HandleClock);
  midiA.setHandleStart(HandleStart);
  midiA.setHandleStop(HandleStop);
  midiA.begin(MIDI_CHANNEL_OMNI);

  // *** WAVESHAPER ***
  //createWaveShaper(waveShapeAmount);
  createWaveShaper();

  // *** OSC 1 & 2 VOLUME ***
  createOsc1Volume();
  createOsc2Volume();

  // *** GAIN ***
  createGainTable();

  // *** SD CARD ***
  if (!sd.begin(chipSelect, SPI_FULL_SPEED))
  {
    lcd.setCursor(0, 1);
    lcd.print("*SD Unavailable*");
  }
  else
  {
    loadSettings();
    getFirstFile();
  }

  // clear the patch buffer
  for (uint16_t i = 0; i < 1900; i++)
    patchBuffer[i] = 0;

  // clear the sequencer bank buffer
  for (uint16_t i = 0; i < 1600; i++)
    seqBankBuffer[i] = 0;
}

// LOOP.ino
void loop()
{
  midiA.read();
  checkForClock();                                                                                             // are we receiving MIDI clock?
  sendMidi();                                                                                                  // we have to take the serial messages out of the interrupt callbacks - will definitely impact MIDI timing though ;^/
  checkSwitches();                                                                                             // gets the current state of the buttons - defined in BUTTONS
  handlePresses();                                                                                             // what to do with button presses - defined in BUTTONS
  checkKeyboard();                                                                                             // checks the front-panel keyboard
  currentEnvelope();                                                                                           // defined in ENVELOPE
  getPots();                                                                                                   // update the pot values - defined in POTS
  getMenu();                                                                                                   // defined in UI
  adjustValues();                                                                                              // defined in POTS
  updateValues();                                                                                              // defined in UI - only executes if the variable valueChange is set to true
  createSquareTable(constrain((pulseWidth + velPw), ((WAVE_SAMPLES / 2) - 10) * -1, (WAVE_SAMPLES / 2) - 10)); // have to call this in the loop for modulation - can't call it at lfo frequency
  arrowAnim();                                                                                                 // animate the arrow
  seqBlinker();                                                                                                // blink the selected step in the sequencer
  updateLED();                                                                                                 // turn the LED on or off
}

// ARP.ino
void arpNextStep()
{
  if (arpLength != 0)
  {
    if (arpPosition == 0)
      arpOctaveCounter = ((arpOctaveCounter + 1) < arpOctaves) ? arpOctaveCounter + 1 : 0;
    if (arpIncrement > 0 && arpIncrement < 4)
      arpPosition = (arpPosition + arpIncrement) % arpLength;
    else
    {
      switch (arpIncrement)
      {
      case 0: // random
        arpPosition = random(0, arpLength);
        break;

      case 4: // 2 forward, 1 back

        static boolean toggle = 0;
        static boolean first = true;
        if (lastArpLength != arpLength)
        {
          first = true;
          lastArpLength = arpLength;
        }
        if (first == true)
        {
          arpPosition = 0;
          first = false;
          toggle = 0;
        }
        else
        {
          if (!toggle)
          {
            if ((arpPosition + 2) <= arpLength - 1)
              arpPosition += 2;
            else
            {
              arpPosition = arpLength - 1;
              first = true;
            }
            toggle = true;
          }
          else if (toggle) // toggle is true
          {
            if (arpPosition == arpLength - 1)
              first = true;
            arpPosition -= 1;
            toggle = false;
          }
        }
        break;
      }
    }
    if (!monoMode)
    {
      voice[0] = sortedArpList[arpPosition] + (arpOctaveCounter * 12);
      for (int i = 0; i < 3; i++)
        voice[i + 1] = 255;
    }
    else
    {
      for (byte j = 0; j < 4; j++)
      {
        if (j < unison + 1)
          voice[0 + j] = sortedArpList[arpPosition] + (arpOctaveCounter * 12);
        else
          voice[0 + j] = 255;
      }
    }
    noteTrigger();
    arpReleasePulse = (pulseCounter + arpNoteDur) % (currentDivision * 2);
    arpReleased = false;
    arpSendMidiNoteOn = true; // send a MIDI note
  }
  else
    lastArpLength = 0;
}

void sortArp()
{
  if (!midiMode)
  {
    if (arpForward)
    {
      for (byte j = 0; j < arpLength; j++)
        sortedArpList[j] = rawArpList[j];
    }
    else // backward
    {
      for (byte j = 0; j < arpLength; j++)
        sortedArpList[j] = rawArpList[(arpLength - 1) - j];
    }
  }
  else // we're in MIDI mode
  {
    arpLength = 0;
    for (int i = 0; i < 10; i++)
    {
      if (rawArpList[i] != 255)
        arpLength++;
    }
    if (arpLength) // ie arpLength != 0 - don't bother to sort if arpLength is 0
    {
      if (arpForward)
      {
        int tmpArpList[10];
        for (byte m = 0; m < 10; m++)
        {
          tmpArpList[m] = rawArpList[m];
        }
        int lowest = 255;
        for (int j = 0; j < arpLength; j++)
        {
          for (int k = 0; k < 10; k++)
          {
            if (tmpArpList[k] < 255 && tmpArpList[k] < lowest)
            {
              lowest = (rawArpList[k]);
            }
          }
          sortedArpList[j] = lowest;
          for (byte n = 0; n < 10; n++)
          {
            if (tmpArpList[n] == lowest)
              tmpArpList[n] = 255;
          }
          lowest = 255;
        }
      }
      else // backwards
      {
      }
    }
  }
}

// BUTTONS.ino
void checkSwitches()
{
  static byte previousstate[NUMBUTTONS];
  static byte currentstate[NUMBUTTONS];
  static long lasttime;
  byte index;

  if (millis() < lasttime)
  {
    // we wrapped around, lets just try again
    lasttime = millis();
  }

  if ((lasttime + DEBOUNCE) > millis())
  {
    // not enough time has passed to debounce
    return;
  }
  // ok we have waited DEBOUNCE milliseconds, lets reset the timer
  lasttime = millis();

  for (index = 0; index < NUMBUTTONS; index++) // when we start, we clear out the "just" indicators
  {
    justreleased[index] = 0;
    justpressed[index] = 0;

    currentstate[index] = digitalRead(buttons[index]); // read the button

    if (currentstate[index] == previousstate[index])
    {
      if ((pressed[index] == LOW) && (currentstate[index] == LOW))
      {
        // just pressed
        justpressed[index] = 1;
      }
      else if ((pressed[index] == HIGH) && (currentstate[index] == HIGH))
      {
        // just released
        justreleased[index] = 1;
      }
      pressed[index] = !currentstate[index]; // remember, digital HIGH means NOT pressed
    }

    previousstate[index] = currentstate[index]; // keep a running tally of the buttons
  }
}

void clearJust()
{
  for (byte index = 0; index < NUMBUTTONS; index++) // when we start, we clear out the "just" indicators
  {
    justreleased[index] = 0;
    justpressed[index] = 0;
  }
}

void handlePresses()
{
  unSplash(); // check if we're on the splash page and deal with it

  if (pressed[13] && justpressed[14]) // fine adjust increment
  {
    if ((*adjustValue + increment) <= upperLimit)
      *adjustValue += increment;
    else
      *adjustValue = lowerLimit;

    incDecSpecials();
    shiftL = true;
    clearJust();
  }

  if (pressed[14] && justpressed[13]) // fine adjust decrement
  {
    if ((*adjustValue - increment) >= lowerLimit)
      *adjustValue -= increment;
    else
      *adjustValue = upperLimit;

    incDecSpecials();
    shiftR = true;
    clearJust();
  }

  if (justreleased[14]) // enter key
  {
    if (!shiftL && !shiftR)
    {
      switch (menu)
      {
      case 0: // SPLASH/MAIN
        switch (mainMenu)
        {
        case 0:                      // SYNTH
          if (synPatchLoadSave == 0) // LOAD
          {
            menu = 70;
            gotoRootDir();
            getDirCount();
            assignIncrementButtons(&dirChoice, 1, dirCount, 1);
          }
          else // SAVE
          {
            gotoRootDir();
            menu = 80;
            getDirCount();
            assignIncrementButtons(&dirChoice, 1, dirCount, 1);
          }
          valueChange = true;
          break;

        case 2:                     // SEQUENCER
          if (seqBankLoadSave == 0) // LOAD
          {
            gotoRootDir();
            menu = 250;
            getDirCount();
            assignIncrementButtons(&dirChoice, 1, dirCount, 1);
          }
          else // SAVE
          {
            gotoRootDir();
            menu = 260;
            getDirCount();
            assignIncrementButtons(&dirChoice, 1, dirCount, 1);
          }
          valueChange = true;
          break;

        case 3: // SETTINGS
          if (settingsConfirm)
          {
            saveSettings();
            settingsConfirm = false;
          }
          else
          {
            settingsConfirm = true;
            updateMenu();
            lcd.setCursor(0, 1);
            lcd.print("    Overwrite?  ");
          }
          break;
        }
        break;

      case 10: // OSC1
        if (osc1WaveType == 4)
        {
          menu = 11;
          gotoRootDir();
          getDirCount();
          assignIncrementButtons(&dirChoice, 1, dirCount, 1);
          valueChange = true;
        }
        else if (osc1WaveType == 3)
        {
          menu = 12;
          lockPot(5);
          assignIncrementButtons(&uiPulseWidth, -285, 285, 2);
          valueChange = true;
          clearLCD();
        }
        break;

      case 11: // OSC1 - choose user waveshape
        if (!inFolder)
        {
          setFolder();
          getDirCount();
          assignIncrementButtons(&dirChoice, 1, dirCount, 1);
          justEnteredFolder = true;
        }
        break;

      case 20: // OSC2
        if (osc2WaveType == 5)
        {
          menu = 21;
          gotoRootDir();
          getDirCount();
          assignIncrementButtons(&dirChoice, 1, dirCount, 1);
          valueChange = true;
        }
        else if (osc2WaveType == 3)
        {
          menu = 22;
          lockPot(5);
          assignIncrementButtons(&uiPulseWidth, -285, 285, 2);
          valueChange = true;
          clearLCD();
        }
        break;

      case 21: // OSC2 - choose user waveshape
        if (!inFolder)
        {
          setFolder();
          getDirCount();
          assignIncrementButtons(&dirChoice, 1, dirCount, 1);
          justEnteredFolder = true;
        }
        break;

      case 50:
        if (lfoShape == 6)
        {
          menu = 51;
          gotoRootDir();
          getDirCount();
          assignIncrementButtons(&dirChoice, 1, dirCount, 1);
          valueChange = true;
        }
        break;

      case 51: // LFO - choose user waveshape
        if (!inFolder)
        {
          setFolder();
          getDirCount();
          assignIncrementButtons(&dirChoice, 1, dirCount, 1);
          justEnteredFolder = true;
        }
        break;

      case 70: // LOAD PATCH
        if (!inFolder)
        {
          setFolder();
          getDirCount();
          assignIncrementButtons(&dirChoice, 1, dirCount, 1);
          justEnteredFolder = true;
        }
        lockPot(5);
        break;

      case 80: // SAVE PATCH
        if (!inFolder)
        {
          setFolder();
          justEnteredFolder = true;
        }
        else if (saveConfirm)
        {
          saveConfirm = false;
          savePatch();
        }
        else
        {
          if (strcmp(fileName, saveName) == 0)
            savePatch();
          else
          {
            saveConfirm = true;
            lcd.setCursor(4, 1);
            lcd.print("Overwrite?  ");
          }
        }
        lockPot(5);
        break;

      case 200: // SEQUENCE TRIGGER
        menu = 210;
        valueChange = true;
        break;

      case 210: // SEQUENCE EDIT
      {
        if (!midiMode)
        {
          boolean keyNotes = false;
          for (byte i = 0; i < 13; i++)
          {
            if (pressed[i])
              keyNotes = true;
          }
          if (keyNotes)
          {
            updateSeqNotes();
            seqEditStep = (seqEditStep + 1) % 16; // increment the edit step
          }
          else // no front pane keyboard keys are held
          {
            menu = 211;
            assignIncrementButtons(&seq[currentSeq].voice[0][seqEditStep], -39, 48, 1);
            clearLCD();
            valueChange = true;
          }
        }
        else // MIDI mode
        {
          if (updateSeqNotes())                   // will return true if there were notes held and added to sequencer
            seqEditStep = (seqEditStep + 1) % 16; // increment the edit step
          else
          {
            menu = 211;
            assignIncrementButtons(&seq[currentSeq].voice[0][seqEditStep], -39, 48, 1);
            clearLCD();
            valueChange = true;
          }
        }
      }
      break;

      case 250: // LOAD SEQ BANK
        if (!inFolder)
        {
          setFolder();
          getDirCount();
          assignIncrementButtons(&dirChoice, 1, dirCount, 1);
          justEnteredFolder = true;
        }
        else if (seqBankLoaded)
        {
          unpackSeqBankBuffer();
          lcd.setCursor(4, 1);
          lcd.print("Loaded!     ");
        }
        lockPot(5);
        break;

      case 260: // SAVE SEQ BANK
        if (!inFolder)
        {
          setFolder();
          justEnteredFolder = true;
        }
        else if (saveConfirm)
        {
          saveConfirm = false;
          saveBank();
        }
        else
        {
          if (strcmp(fileName, saveName) == 0)
            saveBank();
          else
          {
            saveConfirm = true;
            lcd.setCursor(4, 1);
            lcd.print("Overwrite?  ");
          }
        }
        lockPot(5);
        break;
      }
    }
    else if (shiftR)
      shiftR = false;
    clearJust();
  }

  if (justreleased[13]) // back key
  {
    if (!shiftL && !shiftR)
    {
      switch (menu)
      {
      case 11: // OSC1 - choose user waveshape
        if (inFolder)
        {
          setFolder();
          getDirCount();
        }
        else
        {
          menu = 10;
          assignIncrementButtons(&osc1WaveType, 0, 7, 1);
          valueChange = true;
          clearLCD();
        }
        break;

      case 12: // Squ Pulse Width
        menu = 10;
        assignIncrementButtons(&osc1WaveType, 0, 7, 1);
        valueChange = true;
        lockPot(5);
        clearLCD();
        break;

      case 21: // OSC2 - choose user waveshape
        if (inFolder)
        {
          setFolder();
          getDirCount();
        }
        else
        {
          menu = 20;
          assignIncrementButtons(&osc2WaveType, 0, 7, 1);
          valueChange = true;
          clearLCD();
        }
        break;

      case 22: // Squ Pulse Width
        menu = 20;
        assignIncrementButtons(&osc2WaveType, 0, 7, 1);
        valueChange = true;
        lockPot(5);
        clearLCD();
        break;

      case 51: // LFO - choose user waveshape
        if (inFolder)
        {
          setFolder();
          getDirCount();
        }
        else
        {
          menu = 50;
          assignIncrementButtons(&lfoShape, 0, 6, 1);
          valueChange = true;
          clearLCD();
        }
        break;

      case 70: // LOAD PATCH
        if (inFolder)
        {
          setFolder();
          getDirCount();
          assignIncrementButtons(&dirChoice, 1, dirCount, 1);
        }
        else
        {
          menu = 0;
          valueChange = true;
          clearLCD();
          updateMenu();
        }
        lockPot(5);
        break;

      case 80: // SAVE PATCH
        if (inFolder)
        {
          setFolder();
          getDirCount();
          assignIncrementButtons(&dirChoice, 1, dirCount, 1);
        }
        else
        {
          menu = 0;
          valueChange = true;
          clearLCD();
          updateMenu();
        }
        lockPot(5);
        break;

      case 200: // SEQUENCE TRIGGER
        seqPlayStop();
        valueChange = true;
        break;

      case 210: // SEQUENCE EDIT
        seqPlayStop();
        valueChange = true;
        break;

      case 211: // SEQUENCE EDIT NOTES
        menu = 210;
        assignIncrementButtons(&seqEditStep, 0, 15, 1);
        noteRelease();
        clearLCD();
        valueChange = true;
        break;

      case 250: // LOAD SEQ BANK
        if (inFolder)
        {
          setFolder();
          getDirCount();
          assignIncrementButtons(&dirChoice, 1, dirCount, 1);
        }
        else
        {
          menu = 0;
          valueChange = true;
          clearLCD();
          updateMenu();
        }
        lockPot(5);
        break;

      case 260: // SAVE SEQ BANK
        if (inFolder)
        {
          setFolder();
          getDirCount();
          assignIncrementButtons(&dirChoice, 1, dirCount, 1);
        }
        else
        {
          menu = 0;
          valueChange = true;
          clearLCD();
          updateMenu();
        }
        lockPot(5);
        break;
      }
    }
    else if (shiftL)
      shiftL = false;
    clearJust();
  }

  switch (menu)
  {
  case 200: // SEQ TRIGGER
    for (byte i = 0; i < 8; i++)
    {
      if (justpressed[whiteButtons[i]])
      {
        if (!seqRunning)
        {
          sourceSeq = currentSeq;
          currentSeq = i;
          selectedSeq = i;
          destinationSeq = selectedSeq;
          seqUpdateDisplay = true;
          valueChange = true;
        }
        else
        {
          selectedSeq = i;
          sourceSeq = currentSeq;
          destinationSeq = selectedSeq;
          valueChange = true;
        }
        if (midiTriggerOut)
          midiA.sendNoteOn(midiTrigger[i], 127, midiTriggerChannel);
        clearJust();
        longPress = millis();
        copied = false;
      }
      if (pressed[whiteButtons[i]])
      {
        if ((millis() - longPress) > 500 && !copied)
          copySeq();
      }
    }
    for (byte i = 0; i < 5; i++)
    {
      if (pressed[14])
      {
        if (justpressed[1])
        {
          clearSeq();
          clearJust();
          valueChange = true;
          shiftR = true;
        }
      }
      else if (justpressed[blackButtons[i]])
      {
        switch (i)
        {
        case 0:
          seq[currentSeq].transpose--;
          if (seqRunning)
            seqUpdateDisplay = true;
          else
            valueChange = true;
          break;
        case 1:
          seq[currentSeq].transpose++;
          if (seqRunning)
            seqUpdateDisplay = true;
          else
            valueChange = true;
          break;
          clearJust();
        }
      }
    }
    break;

  case 210: // SEQ EDIT
    break;

  case 211: // SEQ NOTE ENTRY
    break;
  }
}

void checkKeyboard()
{
  for (byte i = 0; i < 13; i++)
  {
    if (justpressed[i])
    {
      if (midiMode) // switch to listening to notes from front-panel keyboard
      {
        midiMode = false;
        if (lastFilterCutoff != 0)
          filterCutoff = lastFilterCutoff;
        clearHeld();
      }
      //clearJust();
    }
  }
  if (!midiMode)
  {
    if (soundKeys && menu != 200)
    {
      if (!monoMode) // ie. monoMode = 0
      {
        voiceCounter = 4;            // how mnany voices are used?
        for (byte i = 0; i < 4; i++) // check if a previously held note has been released
        {
          if (voice[i] != 255)
          {
            if (!pressed[voice[i]])
            {
              keyAssigned[voice[i]] = false;
              voice[i] = 255;
              voiceCounter--;
              if (midiOut && keysOut)
                midiA.sendNoteOff(keyboardOut[i], outVelocity, midiChannel);
            }
          }
          else //
            voiceCounter--;
        }

        if (voiceCounter == 0 && lastVoiceCount != 0)
          noteRelease();

        for (byte i = 0; i < 13; i++) // assign note to empty voice
        {
          if (!keyAssigned[i]) // if this key isn't already assigned, look for an empty voice for it
          {
            if (pressed[i])
            {
              byte j = 0;
              while (voice[j] != 255 && j < 4) // go to the next voice if this voice isn't free
                j++;
              if (j < 4)
              {
                if (midiOut && keysOut)
                {
                  keyboardOut[j] = i + 60;
                  midiA.sendNoteOn(keyboardOut[j], keyVelocity, midiChannel);
                }
                voice[j] = i;
                keyAssigned[i] = true;
                noteTrigger();
                setVeloModulation(keyVelocity);
                if (voiceCounter == 0)
                {
                  voiceCounter++;
                  for (byte k = 0; k < 4; k++)
                  {
                    if (k != j)
                      muteVoice[k] = true;
                  }
                }
              }
            }
          }
        }
        lastVoiceCount = voiceCounter;
      }
      else // monoMode
      {
        byte highestNote = 255;
        byte lowestNote = 255;
        static byte previousNote = 255;
        switch (monoMode)
        {
        case 1: // highest note priority
          for (byte i = 0; i < 13; i++)
          {
            if (pressed[i])
              highestNote = i;
          }
          if (voice[0] != highestNote)
          {
            voice[0] = highestNote;
            if (unison)
            {
              for (byte j = 0; j < unison; j++)
                voice[1 + j] = highestNote;
            }
            if (voice[0] == 255)
            {
              if (unison)
              {
                for (byte j = 0; j < unison; j++)
                  voice[1 + j] = 255;
              }
              noteRelease();
              if (midiOut && keysOut)
                midiA.sendNoteOff(keyboardOut[0], outVelocity, midiChannel);
            }
            else
            {
              if (midiOut && keysOut)
                midiA.sendNoteOff(keyboardOut[0], outVelocity, midiChannel);
              noteTrigger();
              setVeloModulation(keyVelocity);
              if (midiOut && keysOut)
              {
                keyboardOut[0] = highestNote + 60;
                midiA.sendNoteOn(keyboardOut[0], keyVelocity, midiChannel);
              }
            }
          }
          break;
        case 2: // lowest note priority
          for (byte i = 0; i < 13; i++)
          {
            if (pressed[12 - i])
              lowestNote = 12 - i;
          }
          if (voice[0] != lowestNote)
          {
            voice[0] = lowestNote;
            if (unison)
            {
              for (byte j = 0; j < unison; j++)
                voice[1 + j] = lowestNote;
            }
            if (voice[0] == 255)
            {
              if (unison)
              {
                for (byte j = 0; j < unison; j++)
                  voice[1 + j] = 255;
              }
              noteRelease();
              if (midiOut && keysOut)
                midiA.sendNoteOff(keyboardOut[0], outVelocity, midiChannel);
            }
            else
            {
              if (midiOut && keysOut)
                midiA.sendNoteOff(keyboardOut[0], outVelocity, midiChannel);
              noteTrigger();
              setVeloModulation(keyVelocity);
              if (midiOut && keysOut)
              {
                keyboardOut[0] = lowestNote + 60;
                midiA.sendNoteOn(keyboardOut[0], keyVelocity, midiChannel);
              }
            }
          }
          break;
        case 3: // last note priority
          for (byte i = 0; i < 13; i++)
          {
            if (justpressed[i])
            {
              if (voice[0] != 255)
              {
                if (midiOut && keysOut)
                  midiA.sendNoteOff(keyboardOut[0], outVelocity, midiChannel);
              }
              voice[0] = i;
              if (unison)
              {
                for (byte j = 0; j < unison; j++)
                  voice[1 + j] = i;
              }
              previousNote = voice[0];
              noteTrigger();
              setVeloModulation(keyVelocity);
              if (midiOut && keysOut)
              {
                keyboardOut[0] = i + 60;
                midiA.sendNoteOn(keyboardOut[0], keyVelocity, midiChannel);
              }
              justpressed[i] = 0;
            }
          }
          for (byte i = 0; i < 13; i++)
          {
            if (justreleased[i])
            {
              if (voice[0] == i)
              {
                voice[0] = 255;
                if (unison)
                {
                  for (byte j = 0; j < unison; j++)
                    voice[1 + j] = 255;
                }
                noteRelease();
                if (midiOut && keysOut)
                  midiA.sendNoteOff(keyboardOut[0], outVelocity, midiChannel);
              }
            }
          }
          if (voice[0] == 255)
          {
            for (byte i = 0; i < 13; i++)
            {
              if (pressed[i])
              {
                highestNote = i;
                if (highestNote > previousNote)
                  break;
              }
            }
            if (highestNote != 255)
            {
              voice[0] = highestNote;
              if (unison)
              {
                for (byte j = 0; j < unison; j++)
                  voice[1 + j] = highestNote;
              }
              noteTrigger();
              setVeloModulation(keyVelocity);
              if (midiOut && keysOut)
              {
                keyboardOut[0] = highestNote + 60;
                midiA.sendNoteOn(keyboardOut[0], keyVelocity, midiChannel);
              }
            }
          }
          break;
        }
      }
    }
    else if (arp) // arp mode is active
    {
      arpLength = 0;
      for (int i = 0; i < 13; i++)
      {
        if (pressed[i] && arpLength < 10)
        {
          rawArpList[arpLength] = i;
          arpLength++;
        }
      }
      if (arpLength == 0)
      {
        for (int i = 0; i < 4; i++)
          voice[i] = 255;
      }
      else
        sortArp();
    }
  }
}

void assignIncrementButtons(int *adjVar, int low, int up, int inc) // variable address, lower limit, upper limit, increment
{
  adjustValue = adjVar;
  lowerLimit = low;
  upperLimit = up;
  increment = inc;
}

void incDecSpecials()
{
  // special cases
  if (adjustValue == &dirChoice && dirCount != 0)
    prepNextChoice();

  else if (adjustValue == &osc1WaveType)
    setOsc1WaveType(*adjustValue);

  else if (adjustValue == &osc2WaveType)
    setOsc2WaveType(*adjustValue);

  else if (adjustValue == &filterResonance)
    setFilterResonance(filterResonance);

  else if (adjustValue == &filterType)
    setFilterType(filterType);

  else if (adjustValue == &lfoShape)
    setLfoShape(lfoShape);

  else if (adjustValue == &tmpLfoRate)
  {
    if (lfoLowRange)
    {
      assignIncrementButtons(&tmpLfoRate, 0, 63, 1);
      lfoRate = 64 - tmpLfoRate;
      userLfoRate = lfoRate;
    }
    else // high range
    {
      assignIncrementButtons(&tmpLfoRate, 0, 1023, 4);
      lfoRate = 1024 - tmpLfoRate; // 1024 here because we don't want the rate to ever be 0;
      userLfoRate = lfoRate;
    }
    velLfoRate = lfoRate;
  }

  else if (adjustValue == &mainMenu)
    updateMenu();

  else if (adjustValue == &synPatchLoadSave)
    updateMenu();

  else if (adjustValue == &seqBankLoadSave)
    updateMenu();

  else if (adjustValue == &arp)
    clearHeld();

  else if (adjustValue == &bpm)
  {
    setBpm();
    if (menu == 220)
      seq[currentSeq].bpm = bpm;
  }

  else if (adjustValue == &arpForward)
    sortArp();

  else if (adjustValue == &osc1Volume)
    createOsc1Volume();

  else if (adjustValue == &osc2Volume)
    createOsc2Volume();

  else if (adjustValue == &shaperType)
    createWaveShaper();

  else if (adjustValue == &shaperType1PotVal)
  {
    float tmp = (float)shaperType1PotVal / 1024;
    waveShapeAmount = tmp;
    createWaveShaper();
  }

  else if (adjustValue == &waveShapeAmount2)
    createWaveShaper();

  else if (adjustValue == &gainAmountPotVal)
  {
    float tmp = (float)gainAmountPotVal / 1024;
    tmp += 1.0;
    gainAmount = tmp;
    createGainTable();
  }

  else if (adjustValue == &midiSync)
    setSyncType();

  // evaluate seperately for the sake of the else
  if (adjustValue == &menuChoice)
  {
    lockTimer = millis();
    switch (mainMenu)
    {
    case 0: // synth
      menu = synthMenu[*adjustValue];
      break;
    case 1: // arp
      menu = arpMenu[*adjustValue];
      break;
    case 2: // sequencer
      menu = seqMenu[*adjustValue];
      break;
    }
    updateMenu();
  }
  else
  {
    valueChange = true;
    lockPot(5);
  }
}

void unSplash()
{
  if (splash)
  {
    if (justpressed[13] || justpressed[14])
    {
      if (justpressed[13])
        shiftL = true;
      else if (justpressed[14])
        shiftR = true;
      customCharacters();
      splash = false;
      updateMenu();
      clearJust();
    }
  }
}

void clearHeld() // makes sure no keys or voices are assigned
{
  envelopeProgress = 255;
  for (byte j = 0; j < 4; j++)
    voice[j] = 255;
  for (byte k = 0; k < 13; k++)
    keyAssigned[k] = false;
}

// CHARACTERS.ino
void customCharacters()
{
  lcd.createChar(0, arrow1);
  lcd.createChar(1, arrow2);
  lcd.createChar(2, arrow3);
  lcd.createChar(3, arrow4);
  lcd.createChar(4, Note);
  lcd.createChar(5, Rest);
  lcd.createChar(6, Mute);
  lcd.createChar(7, Tie);
}

// CLOCK.ino

// *** 96PPQ INTERNAL CLOCK ***

void clockHandler()
{
  if (pulseCounter == 0 && lfoSync)
  {
    lfo8thSyncCounter++;
    if (lfo8thSync == lfo8thSyncCounter)
    {
      lfoSyncCounter = 0;
      lfoIndex = 0;
      lfo8thSyncCounter == 0;
    }
  }

  if (midiClockOut && clockOutCounter == 0)
    midiA.sendRealTime(midi::Clock); // send a midi clock pulse

  if (clockOutCounter < 3)
    clockOutCounter++;
  else
    clockOutCounter = 0;

  LEDon = false; // make sure it's turned off (unless it gets turn on)

  if (arp)
  {
    soundKeys = false; // we don't want to hear notes directly played from the front panel keyboard
    if (pulseCounter < 12)
      LEDon = true;

    currentDivision = arpDivision[arpDivSelection];

    if ((pulseCounter % currentDivision) == 0)
      arpNextStep();
    else if (pulseCounter == arpReleasePulse && arpReleased == false)
    {
      noteRelease();
      arpReleased = true;
      arpSendMidiNoteOff = true;
    }
  }

  else if (seqRunning)
  {
    soundKeys = false; // we don't want to hear notes directly played from the front panel keyboard
    if (pulseCounter < 12 && (eighthCounter % 2) == 0)
      LEDon = true;

    currentDivision = seqDivision[seq[currentSeq].divSelection];

    if (pulseCounter < currentDivision) // we only want to change the swing setting on the first (longer) 16th of the swing pair
      swingFactor = map(seq[currentSeq].swing, 0, 1023, 0, currentDivision / 2);

    if (pulseCounter == 0 || pulseCounter == currentDivision + swingFactor)
      seqNextStep();
    else if (seqUpdateDisplay)
    {
      seqUpdateDisplay = false;
      valueChange = true;
    }

    if (pulseCounter == seqReleasePulse && seqReleasePulse != 255)
    {
      seqSendMidiNoteOffs = true;
      if (!seqReleased)
      {
        noteRelease();
        seqReleased = true;
      }
      seqReleasePulse = 255;
    }
  }

  else if (!soundKeys)
  {
    soundKeys = true; // play sounds from the front panel keyboard
    noteRelease();
  }

  // advance the pulse counter
  if (!receivingClock)
  {
    if (pulseCounter < (currentDivision * 2) - 1)
      pulseCounter++;
    else
    {
      pulseCounter = 0;
      eighthCounter++;
    }
  }
  else // wait for sync
  {
    if (pulseCounter < (currentDivision * 2) - 1)
      pulseCounter++;
  }
}

void setBpm()
{
  bpmPeriod = 60000000 / bpm / 96;
  Timer5.setPeriod(bpmPeriod).start(); // set the timer to the new bpm
  if (lfoSync)
    updateLfoSyncTarget();
}

// ENVELOPE.ino
void currentEnvelope()
{
  static uint16_t attackVolume = 0;
  static unsigned int lastVolume = 0;
  static unsigned int releaseVolume = 0;
  if (envelopeTrigger)
  {
    portaStartTime = millis();
    portaEndTime = portaStartTime + portamento;
    envelopeProgress = 255;
    if (envelopeVolume > 10)
      envelopeVolume -= 9; // we need to do this to prevent clicking
    else
    {
      attackStartTime = millis();
      envelopeProgress = 0;
      attackVolume = lastVolume;
      for (byte i = 0; i < 4; i++)
      {
        if (muteVoice[i])
        {
          voiceSounding[i] = false;
          muteVoice[i] = false;
        }
      }
      velAmp = tempVelAmp;
      assignVoices();
      envelopeTrigger = false;
    }
  }
  switch (envelopeProgress)
  {
  case 0: // attack
    if ((millis() - attackStartTime) > attackTime)
    {
      envelopeProgress = 1;
      decayStartTime = millis();
    }
    else
    {
      envelopeVolume = map(millis(), attackStartTime, attackStartTime + attackTime, 0, 1023);
      lastVolume = envelopeVolume;
      releaseVolume = envelopeVolume;
      envOsc1Pitch = (envelopeVolume * envOsc1PitchFactor) << 4;
      envOsc2Pitch = (envelopeVolume * envOsc2PitchFactor) << 4;
      envFilterCutoff = (envelopeVolume * envFilterCutoffFactor) >> 12;
      if (envLfoRate > 10)
      {
        lfoRate = map(envelopeVolume, 0, 1023, envLfoRate, userLfoRate);
        velLfoRate = lfoRate;
      }
    }
    break;
  case 1: // decay
    if ((millis() - decayStartTime) > decayTime)
    {
      envelopeProgress = 2;
    }
    else
    {
      envelopeVolume = map(millis(), decayStartTime, decayStartTime + decayTime, 1023, sustainLevel);
      lastVolume = envelopeVolume;
      releaseVolume = envelopeVolume;
      envOsc1Pitch = (envelopeVolume * envOsc1PitchFactor) << 4;
      envOsc2Pitch = (envelopeVolume * envOsc2PitchFactor) << 4;
      envFilterCutoff = (envelopeVolume * envFilterCutoffFactor) >> 12;
      if (envLfoRate > 10)
      {
        lfoRate = map(envelopeVolume, 0, 1023, envLfoRate, userLfoRate);
        velLfoRate = lfoRate;
      }
    }
    break;
  case 2: // sustain
    envelopeVolume = sustainLevel;
    lastVolume = sustainLevel;
    releaseVolume = envelopeVolume;
    envOsc1Pitch = (envelopeVolume * envOsc1PitchFactor) << 4;
    envOsc2Pitch = (envelopeVolume * envOsc2PitchFactor) << 4;
    envFilterCutoff = (envelopeVolume * envFilterCutoffFactor) >> 12;
    if (envLfoRate > 10)
    {
      lfoRate = map(envelopeVolume, 0, 1023, envLfoRate, userLfoRate);
      velLfoRate = lfoRate;
    }
    break;
  case 3: // release
    if ((millis() - releaseStartTime) > releaseTime)
    {
      envelopeProgress = 255;
      lastVolume = 0; // just make sure it's actually at zero
      for (byte i = 0; i < 4; i++)
        voiceSounding[i] = false; // none of the voices are sounding
      clearHeld();
    }
    else
    {
      if (envelopeVolume > 0)
      {
        envelopeVolume = map(millis(), releaseStartTime, releaseStartTime + releaseTime, releaseVolume, 0);
        envOsc1Pitch = (envelopeVolume * envOsc1PitchFactor) << 4;
        envOsc2Pitch = (envelopeVolume * envOsc2PitchFactor) << 4;
        envFilterCutoff = (envelopeVolume * envFilterCutoffFactor) >> 12;
        if (envLfoRate > 10)
        {
          lfoRate = map(envelopeVolume, 0, 1023, envLfoRate, userLfoRate);
          velLfoRate = lfoRate;
        }
      }
      else
      {
        envelopeVolume = 0;
        envOsc1Pitch = 0;
        envOsc2Pitch = 0;
        lfoRate = userLfoRate;
        velLfoRate = lfoRate;
      }
      lastVolume = envelopeVolume;
    }
    break;
  case 255: // the envelope is idle
    break;
  }

  if (loadRampDown)
  {
    loadRampFactor = (loadRampFactor > 5) ? loadRampFactor - 5 : 0;
    if (loadRampFactor == 0)
    {
      loadRampDown = false;
      loadProceed();
    }
  }
  else if (loadRampUp)
  {
    loadRampFactor = (loadRampFactor < 1018) ? loadRampFactor + 5 : 1023;
    if (loadRampFactor == 1023)
      loadRampUp = false;
  }
}

void noteTrigger()
{
  envelopeTrigger = true;
  if (retrigger) // should we retrigger the LFO?
    lfoIndex = 0;
}

void noteRelease()
{
  if (envelopeProgress != 3 && envelopeProgress != 255)
  {
    envelopeProgress = 3; // we're in release phase
    releaseStartTime = millis();
  }
}

// FILTER.ino

// based on Groovuino filter.h http://groovuino.blogspot.tw/

void setFilterCutoff(unsigned char cutoff)
{
  f = cutoff;
  setFeedbackf((int)cutoff);
}

void setFilterResonance(unsigned char resonance)
{
  q = resonance;
  setFeedbackq((int)resonance);
}

void setFilterType(unsigned char filtType)
{
  if (filtType == 0)
    fType = 0; // LP
  if (filtType == 1)
    fType = 1; // BP
  if (filtType == 2)
    fType = 2; // HP
}

inline

    int32_t
    filterNextL(int32_t in)
{
  if (filterBypass)
    return in;
  else
  {
    if (fType == 0)
      in >>= 1; // the lowpass filter seems to need more headroom
    int32_t ret;
    hp = in - bufL0;
    bp = bufL0 - bufL1;
    bufL0 += fxmul(f, (hp + fxmul(fb, bp)));
    bufL1 += fxmul(f, bufL0 - bufL1);
    if (fType == 0)
      ret = bufL1;
    if (fType == 1)
      ret = bp;
    if (fType == 2)
      ret = hp;
    return ret + 2048;
  }
}

/*
inline

int32_t filterNextR(int32_t in)
{
  if (filterBypass)
    return in;
  else
  {
    int32_t ret;
    hp = in - bufR0;
    bp = bufR0 - bufR1;
    bufR0 += fxmul(f,  (hp  + fxmul(fb, bp)));
    bufR1 += fxmul(f, bufR0 - bufR1);
    if (fType == 0) ret = bufR1;
    if (fType == 1) ret = bp;
    if (fType == 2) ret = hp;
    return ret + 2048;
  }
}
*/

void setFeedbackf(long f)
{
  fb = q + fxmul(q, (int)SHIFTED_1 - (f / 128));
}

void setFeedbackq(long q)
{
  fb = q + fxmul(q, (int)SHIFTED_1 - (f / 128));
}

// convert an int into to its fixed representation
inline long fx(int i)
{
  return (i << FX_SHIFT);
}

inline long fxmul(long a, int b)
{
  return ((a * b) >> FX_SHIFT);
}

// LFO.ino

void lfoHandler()
{
  // *** LFO ***
  if (!lfoSync)
  {
    lfoCounter++;
    if (lfoCounter >= velLfoRate)
      updateLFO();
  }
  else // lfo is synced to tempo
  {
    lfoSyncCounter++;
    if (lfoSyncCounter >= lfoSyncTarget)
      updateLFO();
  }

  // for the arrow animation
  static uint16_t arrowCounter = 0;
  arrowCounter++;
  if (arrowCounter >= 2200) // 10 times a second
  {
    arrowFrame = (arrowFrame < 9) ? arrowFrame + 1 : 0;
    arrowCounter = 0;
  }

  // to blink selected steps in sequencer edit mode
  seqBlinkCounter++;
  if (seqBlinkCounter >= 11000) // twice a second
  {
    seqBlink = !seqBlink;
    seqBlinkCounter = 0;
  }

  // *** DISPLAY REFRESH ***
  static uint16_t refreshCounter = 0;
  refreshCounter++;
  if (refreshCounter >= 2200) // 22000 / 10 = 2200, so we refresh the the display 10 times a second
  {
    uiRefresh = true;
    refreshCounter = 0;
  }
}

void updateLFO()
{
  tmpLFO = *(lfoShapePointer + lfoIndex);
  lfoIndex = (lfoIndex < 599) ? lfoIndex + 1 : 0;

  // *** OSC1 PITCH ***
  if (lfoOsc1DetuneFactor < 200)                                  // so the pitch modulation is less dramatic at low levels
    lfoOsc1Detune = ((tmpLFO - 2048) * lfoOsc1DetuneFactor) << 1; // needs to be a positive and negative value
  else
    lfoOsc1Detune = ((tmpLFO - 2048) * lfoOsc1DetuneFactor) << 2; // needs to be a positive and negative value

  // *** OSC2 PITCH ***
  if (lfoOsc2DetuneFactor < 200)
    lfoOsc2Detune = ((tmpLFO - 2048) * lfoOsc2DetuneFactor) << 1;
  else
    lfoOsc2Detune = ((tmpLFO - 2048) * lfoOsc2DetuneFactor) << 2;

  // *** FILTER CUTOFF ***
  tmpCutoff = ((tmpLFO - 2048) * lfoAmount) >> 14;                                              // value centered around 0 (positive and negative)
  setFilterCutoff(constrain((filterCutoff + tmpCutoff + envFilterCutoff + velCutoff), 0, 255)); // center  the LFO amount around the current filter cutoff setting

  // *** AMPLITUDE ***
  lfoAmp = 1023 - (((tmpLFO >> 2) * lfoAmpFactor) >> 10);

  // *** PULSE WIDTH ***
  if (lfoPwFactor > 10)
  {
    pulseWidth = ((tmpLFO - 2048) * lfoPwFactor) >> 13;
  }
  else
    pulseWidth = uiPulseWidth;

  if (envelopeProgress != 255) // the envelope is not idle
  {
    assignVoices(); // have to call this in the LFO so pitch modulation is updated
    // *** OSC1 OCTAVE ***
    osc1OctaveOut = osc1Octave + map(tmpLFO, 0, 4095, osc1OctaveMod * -1, osc1OctaveMod);
    constrain(osc1OctaveOut, 1, 9);

    // *** OSC2 OCTAVE ***
    osc2OctaveOut = osc2Octave + map(tmpLFO, 0, 4095, osc2OctaveMod * -1, osc2OctaveMod);
    constrain(osc2OctaveOut, 1, 9);
  }
  if (!lfoSync)
    lfoCounter = 0;
  else
    lfoSyncCounter = 0;
}

void setLfoShape(byte shape)
{
  switch (shape)
  {
  case 0: // sine
    lfoShapePointer = &nSineTable[0];
    break;
  case 1: // triangle
    lfoShapePointer = &nTriangleTable[0];
    break;
  case 2: // saw
    lfoShapePointer = &nSawTable[0];
    break;
  case 3: // square
    lfoShapePointer = &nSquareTable[0];
    break;
  case 4: // user1
    lfoShapePointer = &nUserTable1[0];
    break;
  case 5: // user2
    lfoShapePointer = &nUserTable2[0];
    break;
  case 6: // user3
    lfoShapePointer = &nUserTable3[0];
    break;
  }
}

void updateLfoSyncTarget()
{
  lfoSyncTarget = bpmPeriod * syncTicks[syncSelector] / 600 / 45; // 600 samples in the lfo lookup table, 45microseconds for each lfo tick at 22kHz
  lfo8thSync = syncTicks[syncSelector] / 48;
}

// MIDI.ino
void HandleNoteOn(byte channel, byte note, byte velocity)
{
  if (channel == midiChannel)
  {
    midiVelocity = velocity;

    if (!midiMode)
    {
      midiMode = true; // switch to listening to notes from incoming MIDI
      envelopeProgress = 255;
      for (byte j = 0; j < 4; j++)
        voice[j] = 255;
      for (byte k = 0; k < 10; k++)
        rawArpList[k] = 255;
    }

    if (!arp)
    {
      if (!monoMode) // 4 voice paraphonic mode
      {
        voiceCounter = 0;
        for (byte i = 0; i < 4; i++)
        {
          if (voice[i] != 255)
            voiceCounter++;
        }
        if (velocity == 0 && voiceCounter != 0)
        {
          for (byte i = 0; i < 4; i++)
          {
            if (voice[i] == note - 60)
            {
              voice[i] = 255;
              voiceCounter--;
            }
          }
          if (voiceCounter == 0)
          {
            envelopeProgress = 3; // we're in release phase
            releaseStartTime = millis();
          }
        }
        else
        {
          setVeloModulation(velocity);
          if (voiceCounter < 4)
          {
            byte j = 0;
            while (voice[j] != 255 && j < 4) // go to the next voice if this voice isn't free
              j++;
            if (j < 4)
            {
              voice[j] = note - 60;
              //showValue(13, 1, voice[j]);
              envelopeTrigger = true;
              if (retrigger) // should we retrigger the LFO?
                lfoIndex = 0;
              if (voiceCounter == 0)
              {
                //voiceCounter++;
                for (byte k = 0; k < 4; k++)
                {
                  if (k != j)
                    muteVoice[k] = true;
                }
              }
              voiceCounter++;
            }
          }
        }
      }
      else // monoMode
      {
        if (velocity != 0)
        {
          if (voice[0] == 255)
            clearCurrentNotes();

          switch (monoMode)
          {
          case 1: // highest note priority
            if (highestNote == 255)
            {
              highestNote = note - 60;
            }
            else if (note - 60 > highestNote)
              highestNote = note - 60;

            if (voice[0] != highestNote)
            {
              previousNote = voice[0];
              voice[0] = highestNote;
              if (unison)
              {
                for (byte i = 0; i < unison; i++)
                  voice[1 + i] = highestNote;
              }
              setVeloModulation(velocity);
              noteTrigger();
              for (byte i = 0; i < 11; i++)
              {
                if (i == 10)
                {
                  currentNotes[0] = highestNote;
                  break;
                }
                else if (currentNotes[i] == 255)
                {
                  LEDon = true;
                  currentNotes[i] = highestNote;
                  break;
                }
              }
            }
            break;
          case 2: // lowest note priority
            if (lowestNote == 255)
            {
              lowestNote = note - 60;
            }
            else if (note - 60 < lowestNote)
              lowestNote = note - 60;

            if (voice[0] != lowestNote)
            {
              previousNote = voice[0];
              voice[0] = lowestNote;
              if (unison)
              {
                for (byte i = 0; i < unison; i++)
                  voice[1 + i] = lowestNote;
              }
              setVeloModulation(velocity);
              noteTrigger();
              for (byte i = 0; i < 11; i++)
              {
                if (i == 10)
                {
                  currentNotes[0] = lowestNote;
                  break;
                }
                else if (currentNotes[i] == 255)
                {
                  LEDon = true;
                  currentNotes[i] = lowestNote;
                  break;
                }
              }
            }
            break;
          case 3: // last note priority
            lastNote = note - 60;
            voice[0] = lastNote;
            if (unison)
            {
              for (byte i = 0; i < unison; i++)
                voice[1 + i] = lastNote;
            }
            setVeloModulation(velocity);
            noteTrigger();
            for (byte i = 0; i < 10; i++)
            {
              if (currentNotes[i] == 255)
              {
                currentNotes[i] = lastNote; // assumes an ordered list;
                break;
              }
            }
            break;
          }
        }
      }
    }
    else //we're in arp mode
    {
      byte counter = 0;
      while (rawArpList[counter] != 255 && counter < 9)
        counter++;
      if (rawArpList[counter] == 255)
      {
        rawArpList[counter] = note - 60;
        sortArp();
      }
    }
  }
  if (channel == midiTriggerChannel)
  {
    for (byte i = 0; i < 8; i++)
    {
      if (note == midiTrigger[i])
      {
        if (seqRunning)
        {
          selectedSeq = i;
        }
        else
        {
          currentSeq = i;
          selectedSeq = i;
        }
      }
    }
  }
}

void HandleNoteOff(byte channel, byte note, byte velocity)
{
  if (channel == midiChannel)
  {
    if (!arp)
    {
      if (!monoMode) // we're in 4-voice paraphonic mode
      {
        voiceCounter = 0;
        for (byte i = 0; i < 4; i++)
        {
          if (voice[i] != 255)
            voiceCounter++;
        }

        for (byte i = 0; i < 4; i++)
        {
          if (voice[i] == note - 60)
          {
            voice[i] = 255;
            voiceCounter--;
          }
        }
        if (voiceCounter == 0)
        {
          envelopeProgress = 3; // we're in release phase
          releaseStartTime = millis();
        }
        //showValue(13, 1, voiceCounter);
      }
      else // we are in mono mode
      {
        int tmp = 255;
        switch (monoMode)
        {
        case 1: // highest note priority
          for (byte i = 0; i < 10; i++)
          {
            if ((note - 60) == currentNotes[i])
              currentNotes[i] = 255;
          }

          for (byte i = 0; i < 10; i++)
          {
            if (currentNotes[i] != 255)
            {
              if (tmp == 255)
                tmp = currentNotes[i];
              else if (currentNotes[i] > tmp) // for higest note
                tmp = currentNotes[i];
            }
          }

          if (tmp != 255) // something is held
          {
            if (voice[0] != tmp)
            {
              voice[0] = tmp;
              if (unison)
              {
                for (byte j = 0; j < unison; j++)
                  voice[1 + j] = tmp;
              }
              highestNote = tmp;
              noteTrigger();
            }
          }
          break;
        case 2: // lowest note priority
          for (byte i = 0; i < 10; i++)
          {
            if ((note - 60) == currentNotes[i])
              currentNotes[i] = 255;
          }

          for (byte i = 0; i < 10; i++)
          {
            if (currentNotes[i] != 255)
            {
              if (tmp == 255)
                tmp = currentNotes[i];
              else if (currentNotes[i] < tmp) // for lowest note
                tmp = currentNotes[i];
            }
          }

          if (tmp != 255) // something is held
          {
            if (voice[0] != tmp)
            {
              voice[0] = tmp;
              if (unison)
              {
                for (byte j = 0; j < unison; j++)
                  voice[1 + j] = tmp;
              }
              lowestNote = tmp;
              noteTrigger();
            }
          }
          break;
        case 3: // last note priority
          byte i = 0;
          while (currentNotes[i] != (note - 60) && i < 10) // find the released note in the list
          {
            i++;
          }
          if (i < 10)
          {
            currentNotes[i] = 255; // remove released not from the list
            while (i < 9)          // reorder the list
            {
              currentNotes[i] = currentNotes[i + 1];
              currentNotes[i + 1] = 255;
              i++;
            }
          }
          i = 0;
          while (currentNotes[i] != 255 && i < 10)
          {
            tmp = currentNotes[i];
            i++;
          }
          if (tmp != 255) // something is held
          {
            if (voice[0] != tmp)
            {
              voice[0] = tmp;
              if (unison)
              {
                for (byte j = 0; j < unison; j++)
                  voice[1 + j] = tmp;
              }
              lowestNote = tmp;
              noteTrigger();
            }
          }
          break;
        }
        if (tmp == 255) // nothing is held
        {
          for (byte j = 0; j < 4; j++)
            voice[0 + j] = 255;
          noteRelease();
          clearCurrentNotes();
        }
      }
    }
    else //we're in arp mode
    {
      byte counter = 0;
      while (rawArpList[counter] != (note - 60) && counter < 9)
        counter++;
      if (rawArpList[counter] == (note - 60))
      {
        rawArpList[counter] = 255;
        sortArp();
      }
    }
  }
}

void HandleClock(void) // what to do for 24ppq clock pulses
{
  if (syncIn)
  {
    lastClock = millis();
    receivingClock = true;
    static unsigned long lastClockInMicros = 0;
    if ((inPulseCounter % 12) == 0 && !seqJustStarted) // on each 8th except the first
    {
      pulseCounter = 0;
      eighthCounter++;
    }
    else
      seqJustStarted = false;

    clockInMicros = micros();
    if (inPulseCounter != 0)
    {
      pulseMicros = micros() - lastClockInMicros;
      tmpBpm[inPulseCounter % 24] = 60000000 / (pulseMicros * 24);
    }
    if (inPulseCounter > 4)
    {
      int totalBpm = 0;
      for (byte i = 0; i < 24; i++)
        totalBpm += tmpBpm[i];

      if (bpm != totalBpm / 24 + 1)
      {
        bpm = totalBpm / 24 + 1; // +1 here because for some reason the BPM reading is off by one
        setBpm();
        if (menu == 230)
          showValue(5, 1, bpm);
      }
    }
    inPulseCounter++;
    lastClockInMicros = clockInMicros;
  }
}

void HandleStart(void)
{
  if (syncIn)
  {
    seqRunning = true;
    seqStep = 0; // rewind the sequence
    inPulseCounter = 0;
    pulseCounter = 0;
    eighthCounter = 0;
    longStep = true;
    cueNextSeq();
    seqJustStarted = true;
  }
}

void HandleStop(void)
{
  if (syncIn)
  {
    seqRunning = false;
    seqSendMidiNoteOffs = true;
    seqStep = 0; // rewind the sequence
    pulseCounter = 0;
    eighthCounter = 0;
    longStep = true;

    noteRelease();
  }
}

void sendMidi()
{
  if (midiOut)
  {
    // ARPEGGIATOR
    if (arpSendMidiNoteOn)
    {
      if (arpMidiOut != 255)
        midiA.sendNoteOff(arpMidiOut, 127, midiChannel);
      arpMidiOut = sortedArpList[arpPosition] + (arpOctaveCounter * 12) + 60;
      midiA.sendNoteOn(arpMidiOut, 127, midiChannel);
      arpSendMidiNoteOn = false;
    }

    else if (arpSendMidiNoteOff)
    {
      midiA.sendNoteOff(arpMidiOut, 127, midiChannel);
      arpMidiOut = 255;
      arpSendMidiNoteOff = false;
    }

    // SEQUENCER
    else if (seqSendMidiNoteOns)
    {
      for (byte i = 0; i < 4; i++)
      {
        if (seqMidiOff[i] == 255 && seqMidiOn[i] != 255)
        {
          midiA.sendNoteOn(seqMidiOn[i] + 60, outVelocity, midiChannel);
          seqMidiOff[i] = seqMidiOn[i];
        }
        else if (seqMidiOn[i] != 255)
        {
          midiA.sendNoteOff(seqMidiOff[i] + 60, 127, midiChannel);
          midiA.sendNoteOn(seqMidiOn[i] + 60, outVelocity, midiChannel);
          seqMidiOff[i] = seqMidiOn[i];
        }
      }
      seqSendMidiNoteOns = false;
    }

    else if (seqSendMidiNoteOffs)
    {
      for (byte i = 0; i < 4; i++)
      {
        if (seqMidiOff[i] != 255)
        {
          midiA.sendNoteOff(seqMidiOff[i] + 60, 127, midiChannel);
          seqMidiOff[i] = 255;
        }
      }
      seqSendMidiNoteOffs = false;
    }
  }
}

void checkForClock()
{
  if (receivingClock)
  {
    if ((millis() - lastClock) > 300)
      receivingClock = false;
  }
}

void checkThru()
{
  if (midiThruType == 0)
    midiA.turnThruOff();
  else if (midiThruType == 1)
    midiA.turnThruOn(midi::Full);
  else if (midiThruType == 2)
    midiA.turnThruOn(midi::DifferentChannel); // in conjunction with MIDI_CHANNEL_OMNI, this only allows system messages though like clock, stop, start
}

void setSyncType()
{
  switch (midiSync)
  {
  case 0:
    syncIn = false;
    midiClockOut = 0;
    break;
  case 1:
    syncIn = false;
    midiClockOut = 1;
    break;
  case 2:
    syncIn = true;
    midiClockOut = 0;
    break;
  }
}

void clearCurrentNotes()
{
  for (byte i = 0; i < 10; i++)
    currentNotes[i] = 255;

  highestNote = 255;
  lowestNote = 255;
  lastNote = 255;
  previousNote = 255;
}

// POTS.ino
void getPots() // read the current pot values
{
  for (byte i = 0; i < 5; i++)
  {
    pot[i] = analogRead(i);
  }

  // see if it's time to lock the pots
  if (lockTimer != 0 && (millis() - lockTimer) > 500) // the pots lock after 500ms
  {
    lockPot(5);
    if (menu != 0)
      valueChange = true;
  }
}

void lockPot(byte Pot) // values of 0 - 4 for each pot and 5 for all
{
  if (Pot == 5)
  {
    for (byte i = 0; i < 5; i++)
      potLock[i] = pot[i];
    lockTimer = 0;
  }
  else
    potLock[Pot] = pot[Pot];
}

boolean unlockedPot(byte Pot) // check if a pot is locked or not
{
  if (potLock[Pot] == 9999)
  {
    if (abs(pot[Pot] - lastPotValue[Pot]) > 10) // the threshold value is 10
    {
      lastPotValue[Pot] = pot[Pot];
      lockTimer = millis();
      if (Pot < 4)
        valueChange = true;
    }
    return true;
  }
  else if (abs(potLock[Pot] - pot[Pot]) > 10) // the threshold value is 10
  {
    potLock[Pot] = 9999;
    lockTimer = millis();
    lastPotValue[Pot] = pot[Pot];
    return true;
  }
  else
    return false;
}

void adjustValues()
{
  switch (menu)
  {
  case 0: // main
    if (unlockedPot(0))
    {
      if (splash)
      {
        customCharacters();
        splash = false;
        updateMenu();
      }
      else
        assignIncrementButtons(&mainMenu, 0, 3, 1);

      if (pot[0] < 1023 / 4) // there are 4 mainMenu items
      {
        if (mainMenu != 0)
        {
          mainMenu = 0;
          updateMenu();
        }
      }
      else if (pot[0] < (1023 / 4) * 2)
      {
        if (mainMenu != 1)
        {
          mainMenu = 1;
          updateMenu();
        }
      }
      else if (pot[0] < (1023 / 4) * 3)
      {
        if (mainMenu != 2)
        {
          mainMenu = 2;
          updateMenu();
        }
      }
      else
      {
        if (mainMenu != 3)
        {
          mainMenu = 3;
          updateMenu();
          settingsConfirm = false;
        }
      }
    }
    else if (unlockedPot(3))
    {
      if (splash)
      {
        customCharacters();
        splash = false;
        mainMenu = 0;
        updateMenu();
      }
      else
        assignIncrementButtons(&synPatchLoadSave, 0, 1, 1);
    }
    switch (mainMenu)
    {
    case 0: // Synth
      if (unlockedPot(3))
      {
        if (pot[3] < 512)
        {
          if (synPatchLoadSave != 0)
          {
            synPatchLoadSave = 0;
            updateMenu();
          }
        }
        else
        {
          if (synPatchLoadSave != 1)
          {
            synPatchLoadSave = 1;
            updateMenu();
          }
        }
      }
      break;
    case 2: // Sequencer
      if (unlockedPot(3))
      {
        assignIncrementButtons(&seqBankLoadSave, 0, 1, 1);

        if (pot[3] < 512)
        {
          if (seqBankLoadSave != 0)
          {
            seqBankLoadSave = 0;
            updateMenu();
          }
        }
        else
        {
          if (seqBankLoadSave != 1)
          {
            seqBankLoadSave = 1;
            updateMenu();
          }
        }
      }
      break;
    }
    break;
  case 10: // OSC1
    if (unlockedPot(0))
    {
      assignIncrementButtons(&osc1WaveType, 0, 7, 1);
      if (pot[0] < (1023 / waveshapes))
        setOsc1WaveType(0); // sine
      else if (pot[0] < (1023 / waveshapes) * 2)
        setOsc1WaveType(1); // triangle
      else if (pot[0] < (1023 / waveshapes) * 3)
        setOsc1WaveType(2); // saw
      else if (pot[0] < (1023 / waveshapes) * 4)
        setOsc1WaveType(3); // square
      else if (pot[0] < (1023 / waveshapes) * 5)
        setOsc1WaveType(4); // user1
      else if (pot[0] < (1023 / waveshapes) * 6)
        setOsc1WaveType(5); // user2
      else if (pot[0] < (1023 / waveshapes) * 7)
        setOsc1WaveType(6); // user3
      else
        setOsc1WaveType(7); // noise
    }

    if (unlockedPot(1))
    {
      assignIncrementButtons(&osc1Octave, 1, 9, 1);
      osc1Octave = map(pot[1], 0, 1023, 1, 9);
      if (osc1Octave != lastOsc1Octave)
      {
        lastOsc1Octave = osc1Octave;
        assignVoices();
      }
    }

    if (unlockedPot(2))
    {
      if (osc1Volume != pot[2])
      {
        osc1Volume = pot[2];
        createOsc1Volume();
      }
      assignIncrementButtons(&osc1Volume, 0, 1023, 4);
    }

    if (unlockedPot(3))
    {
      assignIncrementButtons(&osc1Detune, -24, +24, 1);
      osc1Detune = map(pot[3], 0, 1023, -24, 24);
      if (osc1Detune != lastOsc1Detune)
      {
        lastOsc1Detune = osc1Detune;
        assignVoices();
      }
    }
    break;

  case 11: // OSCILLATORS - set user wave shapes
    if (unlockedPot(3) && dirCount != 0)
    {
      justEnteredFolder = false;
      assignIncrementButtons(&dirChoice, 1, dirCount, 1);
      dirChoice = map(pot[3], 0, 1023, 1, dirCount);
      if (dirChoice != lastDirChoice)
      {
        lastDirChoice = dirChoice;
        prepNextChoice();
      }
    }
    break;

  case 12: // Squ: Pulse Width
    if (unlockedPot(3))
    {
      //valueChange = true;
      uiPulseWidth = map(pot[3], 0, 1023, ((WAVE_SAMPLES / 2) - 15) * -1, ((WAVE_SAMPLES / 2) - 15));
    }
    break;

  case 20: // OSC2
    if (unlockedPot(0))
    {
      assignIncrementButtons(&osc2WaveType, 0, 7, 1);
      if (pot[0] < (1023 / waveshapes))
        setOsc2WaveType(0); // sine
      else if (pot[0] < (1023 / waveshapes) * 2)
        setOsc2WaveType(1); // triangle
      else if (pot[0] < (1023 / waveshapes) * 3)
        setOsc2WaveType(2); // saw
      else if (pot[0] < (1023 / waveshapes) * 4)
        setOsc2WaveType(3); // square
      else if (pot[0] < (1023 / waveshapes) * 5)
        setOsc2WaveType(4); // user1
      else if (pot[0] < (1023 / waveshapes) * 6)
        setOsc2WaveType(5); // user2
      else if (pot[0] < (1023 / waveshapes) * 7)
        setOsc2WaveType(6); // user3
      else
        setOsc2WaveType(7); // noise
    }

    if (unlockedPot(1))
    {
      assignIncrementButtons(&osc2Octave, 1, 9, 1);
      osc2Octave = map(pot[1], 0, 1023, 1, 9);
      if (osc2Octave != lastOsc2Octave)
      {
        lastOsc2Octave = osc2Octave;
        assignVoices();
      }
    }

    if (unlockedPot(2))
    {
      if (osc2Volume != pot[2])
      {
        osc2Volume = pot[2];
        createOsc2Volume();
      }
      assignIncrementButtons(&osc2Volume, 0, 1023, 4);
    }

    if (unlockedPot(3))
    {
      assignIncrementButtons(&osc2Detune, -512 << 11, 512 << 11, 2 << 11);
      if (pot[3] > 502 && pot[3] < 522)
        osc2Detune = 0;
      else
        osc2Detune = int(pot[3] - 512) << 11;
      if (osc2Detune != lastOsc2Detune)
      {
        lastOsc2Detune = osc2Detune;
        assignVoices();
      }
    }
    break;

  case 21: // OSCILLATORS - set user wave shapes
    if (unlockedPot(3) && dirCount != 0)
    {
      justEnteredFolder = false;
      dirChoice = map(pot[3], 0, 1023, 1, dirCount);
      if (dirChoice != lastDirChoice)
      {
        lastDirChoice = dirChoice;
        sd.vwd()->rewind();
        tempCount = 0;
        waveshapeLoaded = false;
      }
    }
    break;

  case 22: // Squ: Pulse Width
    if (unlockedPot(3))
    {
      //valueChange = true;
      uiPulseWidth = map(pot[3], 0, 1023, ((WAVE_SAMPLES / 2) - 15) * -1, ((WAVE_SAMPLES / 2) - 15));
    }
    break;

  case 30: // FILTER
    if (unlockedPot(0))
    {
      assignIncrementButtons(&filterCutoff, 0, 255, 1);
      //valueChange = true;
      float sensVar = (pot[0] >> 2) * 1.0;
      filterCutoff = pow(sensVar / 10, 1.71); //(base, exponent)
    }

    if (unlockedPot(1))
    {
      assignIncrementButtons(&filterResonance, 0, 255, 1);
      filterResonance = pot[1] >> 2;
      setFilterResonance(filterResonance);
    }

    if (unlockedPot(2))
    {
      assignIncrementButtons(&filterType, 0, 2, 1);
      if (pot[2] < 341)
        setFilterType(0);
      else if (pot[2] < 682)
        setFilterType(1);
      else
        setFilterType(2);
    }

    if (unlockedPot(3))
    {
      assignIncrementButtons(&filterBypass, 0, 1, 1); // a toggle value
      if (pot[3] < 512)
        filterBypass = true;
      else
        filterBypass = false;
    }
    break;

  case 40: // ENVELOPE
    if (unlockedPot(0))
    {
      attackTime = constrain(pot[0], 1, 1023); // we don't want to be able to set 0
      assignIncrementButtons(&attackTime, 1, 1023, 4);
    }
    if (unlockedPot(1))
    {
      decayTime = constrain(pot[1], 1, 1023); // we don't want to be able to set 0
      assignIncrementButtons(&decayTime, 1, 1023, 4);
    }
    if (unlockedPot(2))
    {
      sustainLevel = pot[2];
      assignIncrementButtons(&sustainLevel, 0, 1023, 4);
    }
    if (unlockedPot(3))
    {
      releaseTime = constrain(pot[3], 1, 1023); // we don't want to be able to set 0
      assignIncrementButtons(&releaseTime, 1, 1023, 4);
    }
    break;

  case 50: // LFO
    if (unlockedPot(0))
    {
      assignIncrementButtons(&lfoShape, 0, 6, 1);
      if (pot[0] < (1023 / 7))
      {
        if (lfoShape != 0)
        {
          lfoShape = 0; // sine
          setLfoShape(lfoShape);
        }
      }
      else if (pot[0] < (1023 / 7) * 2)
      {
        if (lfoShape != 1)
        {
          lfoShape = 1; // triangle
          setLfoShape(lfoShape);
        }
      }
      else if (pot[0] < (1023 / 7) * 3)
      {
        if (lfoShape != 2)
        {
          lfoShape = 2; // saw
          setLfoShape(lfoShape);
        }
      }
      else if (pot[0] < (1023 / 7) * 4)
      {
        if (lfoShape != 3)
        {
          lfoShape = 3; // square
          setLfoShape(lfoShape);
        }
      }
      else if (pot[0] < (1023 / 7) * 5)
      {
        if (lfoShape != 4)
        {
          lfoShape = 4; // user1
          setLfoShape(lfoShape);
        }
      }
      else if (pot[0] < (1023 / 7) * 6)
      {
        if (lfoShape != 5)
        {
          lfoShape = 5; // user2
          setLfoShape(lfoShape);
        }
      }
      else
      {
        if (lfoShape != 0)
        {
          {
            lfoShape = 6; // user3
            setLfoShape(lfoShape);
          }
        }
      }
    }
    if (unlockedPot(1))
    {
      if (pressed[14])
      {
        shiftR = true;
        lfoSync = true;
        getSyncSelector();
        updateLfoSyncTarget();
      }
      else
      {
        lfoSync = false;
        if (lfoLowRange)
        {
          tmpLfoRate = constrain(pot[1] >> 4, 0, 63);
          assignIncrementButtons(&tmpLfoRate, 0, 63, 1);
          lfoRate = 64 - tmpLfoRate;
          if (userLfoRate != lfoRate)
            userLfoRate = lfoRate;
        }
        else
        {
          tmpLfoRate = constrain(pot[1], 0, 1023); // 1024 here because we don't want the rate to ever be 0;
          assignIncrementButtons(&tmpLfoRate, 0, 1023, 4);
          lfoRate = 1024 - tmpLfoRate;
          if (userLfoRate != lfoRate)
            userLfoRate = lfoRate;
        }
        velLfoRate = lfoRate;
      }
    }
    if (unlockedPot(2))
    {
      assignIncrementButtons(&lfoLowRange, 0, 1, 1);
      if (pot[2] < 512)
      {
        if (lfoRate > 31)
          lfoRate = 31;
        lfoLowRange = true;
      }
      else
        lfoLowRange = false;
    }
    if (unlockedPot(3))
    {
      if (pot[3] < 512)
        retrigger = true;
      else
        retrigger = false;
    }
    break;

  case 51: // LFO - set user wave shapes
    if (unlockedPot(3) && dirCount != 0)
    {
      justEnteredFolder = false;
      dirChoice = map(pot[3], 0, 1023, 1, dirCount);
      if (dirChoice != lastDirChoice)
      {
        lastDirChoice = dirChoice;
        sd.vwd()->rewind();
        tempCount = 0;
        waveshapeLoaded = false;
      }
    }
    break;

  case 60: // MODULATION
    if (unlockedPot(0))
    {
      assignIncrementButtons(&source, 0, 2, 1);
      source = map(pot[0], 0, 1023, 0, 2);
    }

    if (unlockedPot(1))
    {
      switch (source)
      {
      case 0: // SOURCE LFO
        assignIncrementButtons(&destination, 0, 6, 1);
        dest = 1023 / 7; // how many destinations are there
        if (pot[1] < dest)
          destination = 0;
        else if (pot[1] < dest * 2)
          destination = 1;
        else if (pot[1] < dest * 3)
          destination = 2;
        else if (pot[1] < dest * 4)
          destination = 3;
        else if (pot[1] < dest * 5)
          destination = 4;
        else if (pot[1] < dest * 6)
          destination = 5;
        else
          destination = 6;
        break;

      case 1: // SOURCE ENVELOPE
        assignIncrementButtons(&destination, 7, 10, 1);
        dest = 1023 / 4; // how many destinations are there
        if (pot[1] < dest)
          destination = 7;
        else if (pot[1] < dest * 2)
          destination = 8;
        else if (pot[1] < dest * 3)
          destination = 9;
        else
          destination = 10;
        break;

      case 2: // SOURCE VELOCITY
        assignIncrementButtons(&destination, 11, 16, 1);
        dest = 1023 / 6; // how many destinations are there
        if (pot[1] < dest)
          destination = 11;
        else if (pot[1] < dest * 2)
          destination = 12;
        else if (pot[1] < dest * 3)
          destination = 13;
        else if (pot[1] < dest * 4)
          destination = 14;
        else if (pot[1] < dest * 5)
          destination = 15;
        else
          destination = 16;
        break;
      }
    }

    if (unlockedPot(2))
    {
      switch (source)
      {
      case 0: // SOURCE LFO
        switch (destination)
        {
        case 0:
          assignIncrementButtons(&lfoOsc1DetuneFactor, 0, 1023, 4);
          lfoOsc1DetuneFactor = pot[2];
          break;
        case 1:
          assignIncrementButtons(&lfoOsc2DetuneFactor, 0, 1023, 4);
          lfoOsc2DetuneFactor = pot[2];
          break;
        case 2:
          assignIncrementButtons(&osc1OctaveMod, 0, 3, 1);
          osc1OctaveMod = pot[2] >> 8;
          break;
        case 3:
          assignIncrementButtons(&osc2OctaveMod, 0, 3, 1);
          osc2OctaveMod = pot[2] >> 8;
          break;
        case 4:
          assignIncrementButtons(&lfoAmount, 0, 1023, 4);
          lfoAmount = pot[2];
          break;
        case 5:
          assignIncrementButtons(&lfoAmpFactor, 0, 1023, 4);
          lfoAmpFactor = pot[2];
          break;
        case 6:
          assignIncrementButtons(&lfoPwFactor, 0, 1023, 4);
          lfoPwFactor = pot[2];
          break;
        }
        break;

      case 1: // SOURCE ENVELOPE
        switch (destination)
        {
        case 7:
          assignIncrementButtons(&envOsc1PitchFactor, -1023, 1023, 4);
          if (pot[2] > 502 && pot[2] < 522) // snap to center / 0
            envOsc1PitchFactor = 0;
          else
            envOsc1PitchFactor = (pot[2] - 512) << 1; // - 1023 to 1023
          break;
        case 8:
          assignIncrementButtons(&envOsc2PitchFactor, -1023, 1023, 4);
          if (pot[2] > 502 && pot[2] < 522) // snap to center / 0
            envOsc2PitchFactor = 0;
          else
            envOsc2PitchFactor = (pot[2] - 512) << 1; // - 1023 to 1023
        case 9:                                       // FILTER CUTOFF
          assignIncrementButtons(&envFilterCutoffFactor, -1023, 1023, 4);
          if (pot[2] > 502 && pot[2] < 522) // snap to center / 0
            envFilterCutoffFactor = 0;
          else
            envFilterCutoffFactor = (pot[2] - 512) << 1; // - 1023 to 1023
          break;
        case 10: // LFO RATE
          assignIncrementButtons(&envLfoRate, 0, 1023, 4);
          envLfoRate = pot[2];
          break;
        }
        break;

      case 2: // SOURCE VELOCITY
        switch (destination)
        {
        case 11:
          assignIncrementButtons(&velOsc1DetuneFactor, -1023, 1023, 8);
          velOsc1DetuneFactor = map(pot[2], 0, 1023, -1023, 1023);
          break;
        case 12:
          assignIncrementButtons(&velOsc2DetuneFactor, -1023, 1023, 8);
          velOsc2DetuneFactor = map(pot[2], 0, 1023, -1023, 1023);
          break;
        case 13:
          assignIncrementButtons(&velCutoffFactor, 0, 1023, 4);
          velCutoffFactor = pot[2];
          break;
        case 14:
          assignIncrementButtons(&velAmpFactor, 0, 1023, 4);
          velAmpFactor = pot[2];
          break;
        case 15:
          assignIncrementButtons(&velPwFactor, 0, (WAVE_SAMPLES / 2) - 10, 1);
          velPwFactor = map(pot[2], 0, 1023, 0, (WAVE_SAMPLES / 2) - 10);
          break;
        case 16:
          assignIncrementButtons(&velLfoRateFactor, 0, 1023, 4);
          velLfoRateFactor = pot[2];
          break;
        }
        break;
      }
    }
    break;

  case 65: // SHAPER & GAIN
    if (unlockedPot(0))
    {
      assignIncrementButtons(&shaperType, 0, 2, 1);
      int items = 3;
      if (shaperType != (pot[0] / ((1023 / items) + 1)))
      {
        shaperType = pot[0] / ((1023 / items) + 1);
        createWaveShaper();
      }
    }
    if (unlockedPot(1))
    {
      if (shaperType == 1)
      {
        assignIncrementButtons(&shaperType1PotVal, 0, 1023, 4);
        if (pot[1] != shaperType1PotVal)
        {
          shaperType1PotVal = pot[1];
          float tmp = (float)shaperType1PotVal / 1024;
          waveShapeAmount = tmp;
          createWaveShaper();
        }
      }
      else if (shaperType == 2)
      {
        assignIncrementButtons(&waveShapeAmount2, 1, 10, 1);
        int tmp = map(pot[1], 0, 1023, 1, 10);
        if (tmp != waveShapeAmount2)
        {
          waveShapeAmount2 = tmp;
          createWaveShaper();
        }
      }
    }

    if (unlockedPot(2))
    {
      assignIncrementButtons(&bitMuncher, 0, 11, 1);
      int values = 11;
      if (bitMuncher != (pot[2] / ((1023 / values) + 1)))
        bitMuncher = pot[2] / ((1023 / values) + 1);
    }

    if (unlockedPot(3))
    {
      assignIncrementButtons(&gainAmountPotVal, 0, 1023, 4);
      if (pot[3] != gainAmountPotVal)
      {
        gainAmountPotVal = pot[3];
        float tmp = (float)gainAmountPotVal / 1024;
        tmp += 1.0;
        gainAmount = tmp;
        createGainTable();
      }
    }
    break;

  case 68: // MONO & PORTA
    if (unlockedPot(0))
    {
      assignIncrementButtons(&monoMode, 0, 3, 1);
      int values = 4;
      byte tmp = constrain(pot[0] / ((1023 / values) + 1), 0, 3);
      if (monoMode != tmp)
      {
        monoMode = tmp;
        clearCurrentNotes(); // clears the currentNotes table and resets variables that keep track of incoming MIDI notes
      }
    }
    if (unlockedPot(1))
    {
      assignIncrementButtons(&unison, 0, 3, 1);
      int values = 4;
      byte tmp = constrain(pot[1] / ((1023 / values) + 1), 0, 3);
      if (unison != tmp)
        unison = tmp;
    }
    if (unlockedPot(2))
    {
      assignIncrementButtons(&uniSpread, 10000, 60000, 200);
      int tmp = map(pot[2], 0, 1023, 10000, 60000);
      if (uniSpread != tmp)
        uniSpread = tmp;
    }
    if (unlockedPot(3))
    {
      assignIncrementButtons(&portamento, 0, 255, 1);
      if (portamento != constrain(pot[3] >> 2, 0, 255))
        portamento = constrain(pot[3] >> 2, 0, 255);
    }
    break;

  case 70: // LOAD SYNTH PATCH
    if (unlockedPot(3) && dirCount != 0)
    {
      int potSteps = 1023 / dirCount;
      dirChoice = constrain((pot[3] / potSteps) + 1, 1, dirCount);
      //dirChoice = map(pot[3], 0, 1023, 1, dirCount);
      if (dirChoice != lastDirChoice)
      {
        synthPatchLoaded = false;
        lastDirChoice = dirChoice;
        sd.vwd()->rewind();
        tempCount = 0;
        justEnteredFolder = false;
      }
    }
    break;

  case 80: // SAVE SYNTH PATCH
    if (unlockedPot(3) && dirCount != 0)
    {
      int potSteps = 1023 / dirCount;
      dirChoice = constrain((pot[3] / potSteps) + 1, 1, dirCount);
      //dirChoice = map(pot[3], 0, 1023, 1, dirCount);
      if (dirChoice != lastDirChoice)
      {
        lastDirChoice = dirChoice;
        sd.vwd()->rewind();
        tempCount = 0;
      }
    }
    break;

  case 100:             // ARP SETTINGS 1
    if (unlockedPot(0)) // arp on/off
    {
      assignIncrementButtons(&arp, 0, 1, 1);
      if (pot[0] < 512)
      {
        if (arp == true)
        {
          arp = false;
          clearHeld();
        }
      }
      else
        arp = true;
    }
    if (unlockedPot(1)) // time division
    {
      assignIncrementButtons(&arpDivSelection, 0, 6, 1);
      arpDivSelection = map(pot[1], 0, 1023, 0, 6);
    }
    if (unlockedPot(2)) // step duration in pulses
    {
      assignIncrementButtons(&arpNoteDur, 2, 95, 1);
      arpNoteDur = map(pot[2], 0, 1023, 2, 95);
    }
    if (unlockedPot(3)) // set the BPM
    {
      assignIncrementButtons(&bpm, 20, 320, 1);
      bpm = map(pot[3], 0, 1023, 20, 320);
      if (bpm != lastBpm)
      {
        lastBpm = bpm;
        setBpm();
      }
    }
    break;

  case 110:             // ARP SETTINGS 2
    if (unlockedPot(0)) // direction
    {
      assignIncrementButtons(&arpForward, 0, 1, 1);
      if (pot[0] < 512)
      {
        if (arpForward != 0)
        {
          arpForward = 0;
          sortArp();
        }
      }
      else
      {
        if (arpForward != 1)
        {
          arpForward = 1;
          sortArp();
        }
      }
    }
    if (unlockedPot(1)) // type
    {
      assignIncrementButtons(&arpIncrement, 0, 4, 1);
      arpIncrement = map(pot[1], 0, 1023, 0, 4);
    }
    if (unlockedPot(2)) // octaves
    {
      assignIncrementButtons(&arpOctaves, 1, 4, 1);
      arpOctaves = map(pot[2], 0, 1023, 1, 4);
    }
    break;

  case 200: // SEQUENCER TRIGGER
    if (unlockedPot(0))
    {
      if (pot[0] < 512)
      {
        if (seqRunning != 0)
          seqPlayStop();
      }
      else
      {
        if (!seqRunning)
          seqPlayStop();
      }
    }

    if (unlockedPot(1))
    {
      assignIncrementButtons(&seqPlayMode, 0, 5, 1);
      seqPlayMode = map(pot[1], 0, 1023, 0, 5);
    }

    if (unlockedPot(2))
    {
      assignIncrementButtons(&seq[currentSeq].transpose, -36, 36, 1);
      seq[currentSeq].transpose = map(pot[2], 0, 1023, -36, 36);
    }
    break;

  case 210: // SEQUENCER EDIT
    if (unlockedPot(0))
    {
      if (pot[0] < 512)
      {
        if (seqRunning != 0)
          seqPlayStop();
      }
      else
      {
        if (!seqRunning)
          seqPlayStop();
      }
    }
    if (unlockedPot(1))
    {
      clearStep();
      lockPot(1);
      valueChange = true;
    }
    if (unlockedPot(2))
    {
      if (pot[2] < 341)
      {
        seq[currentSeq].tie[seqEditStep] = 0;
        seq[currentSeq].mute[seqEditStep] = 0;
      }
      else if (pot[2] < 682)
      {
        seq[currentSeq].tie[seqEditStep] = 1;
        seq[currentSeq].mute[seqEditStep] = 0;
      }
      else if (pot[2] < 1023)
      {
        seq[currentSeq].tie[seqEditStep] = 0;
        seq[currentSeq].mute[seqEditStep] = 1;
      }
    }
    if (unlockedPot(3))
    {
      assignIncrementButtons(&seqEditStep, 0, 15, 1);
      int tmp = constrain(pot[3] / (1023 / 16), 0, 15);
      if (seqEditStep != tmp)
      {
        seqEditStep = tmp;
        seqBlink = 0;           // so the just selected step is switched off
        seqBlinkCounter = 7000; // so it stays off for just a little bit before it's switched back on
        valueChange = true;
      }
    }
    break;

  case 211: // SEQUENCER EDIT STEP
    if (unlockedPot(0))
    {
      assignIncrementButtons(&seq[currentSeq].voice[0][seqEditStep], -39, 48, 1);
      if (pot[0] < 100)
      {
        seq[currentSeq].voice[0][seqEditStep] = 255;
        valueChange = true;
      }
      else
      {
        int tmpNote = map(pot[0], 100, 1023, 21, 108) - 60;
        if (tmpNote != seq[currentSeq].voice[0][seqEditStep])
        {
          seq[currentSeq].voice[0][seqEditStep] = tmpNote;
          valueChange = true;
        }
      }
    }
    if (unlockedPot(1))
    {
      assignIncrementButtons(&seq[currentSeq].voice[1][seqEditStep], -39, 48, 1);
      if (pot[1] < 100)
      {
        seq[currentSeq].voice[1][seqEditStep] = 255;
        valueChange = true;
      }
      else
      {
        int tmpNote = map(pot[1], 100, 1023, 21, 108) - 60;
        if (tmpNote != seq[currentSeq].voice[1][seqEditStep])
        {
          seq[currentSeq].voice[1][seqEditStep] = tmpNote;
          valueChange = true;
        }
      }
    }
    if (unlockedPot(2))
    {
      assignIncrementButtons(&seq[currentSeq].voice[2][seqEditStep], -39, 48, 1);
      if (pot[2] < 100)
      {
        seq[currentSeq].voice[2][seqEditStep] = 255;
        valueChange = true;
      }
      else
      {
        int tmpNote = map(pot[2], 100, 1023, 21, 108) - 60;
        if (tmpNote != seq[currentSeq].voice[2][seqEditStep])
        {
          seq[currentSeq].voice[2][seqEditStep] = tmpNote;
          valueChange = true;
        }
      }
    }
    if (unlockedPot(3))
    {
      assignIncrementButtons(&seq[currentSeq].voice[3][seqEditStep], -39, 48, 1);
      if (pot[3] < 100)
      {
        seq[currentSeq].voice[3][seqEditStep] = 255;
        valueChange = true;
      }
      else
      {
        int tmpNote = map(pot[3], 100, 1023, 21, 108) - 60;
        if (tmpNote != seq[currentSeq].voice[3][seqEditStep])
        {
          seq[currentSeq].voice[3][seqEditStep] = tmpNote;
          valueChange = true;
        }
      }
    }
    break;

  case 220: // SEQUENCER EDIT STEP
    if (unlockedPot(0))
    {
      assignIncrementButtons(&seq[currentSeq].patternLength, 1, 16, 1);
      seq[currentSeq].patternLength = map(pot[0], 0, 1023, 1, 16);
    }
    if (unlockedPot(1))
    {
      assignIncrementButtons(&seq[currentSeq].noteDur, 0, 1023, 4);
      seq[currentSeq].noteDur = pot[1];
    }
    if (unlockedPot(2))
    {
      assignIncrementButtons(&seq[currentSeq].swing, 0, 1023, 4);
      seq[currentSeq].swing = pot[2];
    }
    if (unlockedPot(3))
    {
      assignIncrementButtons(&bpm, 20, 320, 1);
      bpm = map(pot[3], 0, 1023, 20, 320);
      if (bpm != lastBpm)
      {
        lastBpm = bpm;
        setBpm();
        seq[currentSeq].bpm = bpm;
      }
    }
    break;

  case 230: // SEQUENCER BANK SETTINGS
    if (unlockedPot(0))
    {
      int tmpMode = map(pot[0], 0, 1023, 0, 4);
      if (bankMode != tmpMode)
        bankMode = tmpMode;
      //valueChange = true;
    }
    break;

  case 250: // LOAD SEQ BANK
    if (unlockedPot(3) && dirCount != 0)
    {
      justEnteredFolder = false;
      dirChoice = map(pot[3], 0, 1023, 1, dirCount);
      if (dirChoice != lastDirChoice)
      {
        seqBankLoaded = false;
        lastDirChoice = dirChoice;
        sd.vwd()->rewind();
        tempCount = 0;
      }
    }
    break;

  case 260: // SAVE SEQ BANK
    if (unlockedPot(3) && dirCount != 0)
    {
      dirChoice = map(pot[3], 0, 1023, 1, dirCount);
      if (dirChoice != lastDirChoice)
      {
        lastDirChoice = dirChoice;
        sd.vwd()->rewind();
        tempCount = 0;
      }
    }
    break;

  case 300: // SETTINGS MIDI
    if (unlockedPot(0))
    {
      assignIncrementButtons(&midiOut, 0, 1, 1);
      int tmp = (pot[0] < 512) ? 0 : 1;
      if (midiOut != tmp)
      {
        midiOut = tmp;
        valueChange;
      }
    }
    if (unlockedPot(1))
    {
      assignIncrementButtons(&midiChannel, 1, 16, 1);
      int tmp = map(pot[1], 0, 1023, 1, 16);
      if (midiChannel != tmp)
      {
        midiChannel = tmp;
        valueChange;
      }
    }
    if (unlockedPot(2))
    {
      assignIncrementButtons(&midiThruType, 0, 2, 1);
      byte items = 3;
      int tmp = (pot[2] / (1022 / items) < (items - 1)) ? pot[2] / (1022 / items) : items - 1;
      if (midiThruType != tmp)
      {
        midiThruType = tmp;
        checkThru();
        valueChange;
      }
    }
    if (unlockedPot(3))
    {
      assignIncrementButtons(&midiSync, 0, 2, 1);
      int tmp = (pot[3] / (1027 / 3));
      if (midiSync != tmp)
      {
        midiSync = tmp;
        setSyncType();
      }
    }
    break;

  case 310: // SETTINGS KEYBOARD
    if (unlockedPot(0))
    {
      assignIncrementButtons(&keysOut, 0, 1, 1);
      if (pot[0] < 512)
        keysOut = 0;
      else
        keysOut = 1;
    }
    if (unlockedPot(1))
    {
      assignIncrementButtons(&keyVelocity, 0, 127, 1);
      if (keyVelocity != pot[1] >> 3)
        keyVelocity = pot[1] >> 3;
    }
    break;
  case 320: // // SETTINGS TRIGGER MIDI
    if (unlockedPot(0))
    {
      assignIncrementButtons(&midiTriggerOut, 0, 1, 1);
      if (pot[0] < 512)
        midiTriggerOut = 0;
      else
        midiTriggerOut = 1;
    }
    if (unlockedPot(1))
    {
      assignIncrementButtons(&midiTriggerChannel, 1, 16, 1);
      int tmp = (pot[1] / (1024 / 16)) + 1;
      if (tmp != midiTriggerChannel)
        midiTriggerChannel = tmp;
    }
    if (unlockedPot(2))
    {
      assignIncrementButtons(&editTrigger, 0, 7, 1);
      int tmp = pot[2] / (1024 / 8);
      if (tmp != editTrigger)
        editTrigger = tmp;
    }
    if (unlockedPot(3))
    {
      assignIncrementButtons(&midiTrigger[editTrigger], 0, 127, 1);
      int tmp = pot[3] >> 3;
      if (tmp != midiTrigger[editTrigger])
        midiTrigger[editTrigger] = tmp;
    }
    break;
  case 330: // // SETTINGS GENERAL
    if (unlockedPot(0))
    {
      assignIncrementButtons(&volume, 0, 1023, 4);
      if (pot[0] != volume)
        volume = pot[0];
    }
    break;
  }
}

void getMenu()
{
  switch (mainMenu)
  {
  case 0: // SYNTH
    menuPages = 9;
    if (unlockedPot(4)) // select the menu page
    {
      assignIncrementButtons(&menuChoice, 0, 8, 1);
      int tmp = 1023 / menuPages;
      menuChoice = constrain(pot[4] / tmp, 0, menuPages - 1);
      menu = synthMenu[menuChoice];
      if (menu != lastMenu)
      {
        lastMenu = menu;

        for (byte i = 0; i < 4; i++)
          lockPot(i);
        updateMenu();
        if (splash)
        {
          customCharacters();
          splash = false;
        }
      }
      else if (lastMainMenu != mainMenu)
      {
        lastMainMenu = mainMenu;
        lockPot(5);
      }
    }
    if (menu == 50) // lfo page
      lfoLED = true;
    else
      lfoLED = false;
    break;

  case 1: // ARP
    menuPages = 3;
    if (unlockedPot(4)) // select the menu page
    {
      assignIncrementButtons(&menuChoice, 0, 2, 1);
      int tmp = 1023 / menuPages;
      menuChoice = constrain(pot[4] / tmp, 0, menuPages - 1);
      menu = arpMenu[menuChoice];
      if (menu != lastMenu)
      {
        lastMenu = menu;
        for (byte i = 0; i < 4; i++)
          lockPot(i);
        updateMenu();
      }
    }
    break;

  case 2: // SEQUENCER
    menuPages = 5;
    if (unlockedPot(4)) // select the menu page
    {
      assignIncrementButtons(&menuChoice, 0, 4, 1);
      int tmp = 1023 / menuPages;
      menuChoice = constrain(pot[4] / tmp, 0, menuPages - 1);
      menu = seqMenu[menuChoice];
      if (menu != lastMenu)
      {
        lastMenu = menu;
        for (byte i = 0; i < 4; i++)
          lockPot(i);
        if (menu == 210)
          doSeqBlink = false; // so the page name doesn't blink
        updateMenu();
      }
    }
    break;

  case 3: // SETTINGS
    menuPages = 5;
    if (unlockedPot(4)) // select the menu page
    {
      assignIncrementButtons(&menuChoice, 0, 4, 1);
      int tmp = 1023 / menuPages;
      menuChoice = constrain(pot[4] / tmp, 0, menuPages - 1);
      menu = settingsMenu[menuChoice];
      if (menu != lastMenu)
      {
        lastMenu = menu;
        for (byte i = 0; i < 4; i++)
          lockPot(i);
        updateMenu();
      }
    }
    break;
  }
}

void getSyncSelector()
{
  int lastSyncSelector = syncSelector;
  syncSelector = constrain((pot[1] / 204), 0, 4);
  if (syncSelector == lastSyncSelector)
    lockPot(5);
  valueChange = true;
}

// SD.ino
void getWaveform()
{
  if (dirCount == 0 && dirChecked == false)
  {
    getDirCount();
  }

  else
  {
    while (tempCount < dirChoice)
    {
      file.openNext(sd.vwd(), O_READ);
      tempCount++;
      file.getFilename(fileName);
      if (file.isDir())
      {
        folder = true;
        file.getFilename(folderName);
      }
      else
        folder = false;
      file.close();
    }
    if (tempCount == dirChoice)
    {
      lcd.setCursor(0, 1);
      lcd.print("    ");
      lcd.print(fileName);
      if (folder)
        arrow(strlen(fileName) + 4, 1);
      //lcd.print(char(127));
      lcd.print("            "); // sloppy way to clear excess characters
    }
    if (!waveshapeLoaded)
    {
      // if (
      if (checkExtension(".WAV"))
        //);
        loadWaveshape();
    }
  }
}

void getDirCount()
{
  sd.vwd()->rewind();
  tempCount = 0;
  while (file.openNext(sd.vwd(), O_READ))
  {
    //if (!macFile())
    tempCount++;
    file.close();
  }
  dirCount = tempCount;
  dirChoice = 0;
  sd.vwd()->rewind();
  dirChecked = true;
  valueChange = true;
}

void loadWaveshape()
{
  if (!folder)
  {
    file.open(fileName);
    file.seekSet(44);                 // just after the header data
    file.read(waveShapeBuffer, 1200); // we know our waveeshape files are all 1200 bytes long (ie. 600 * 16 bit ints)
    file.close();
    switch (menu)
    {
    case 11:
      for (int i = 0; i < 600; i++)
        nUserTable1[i] = (waveShapeBuffer[i] >> 4) + 2048;
      break;
    case 21:
      for (int i = 0; i < 600; i++)
        nUserTable2[i] = (waveShapeBuffer[i] >> 4) + 2048;
      break;
    case 51:
      for (int i = 0; i < 600; i++)
        nUserTable3[i] = (waveShapeBuffer[i] >> 4) + 2048;
      break;
    }
    file.close();
    waveshapeLoaded = true;
  }
}

boolean checkExtension(char ext[]) // checks the extension of the file, use argument like ".WAV", ".TB2"
{
  byte nameLength = strlen(fileName);
  if (nameLength < 6)
    return false;
  else
  {
    char lastFour[5];
    for (byte i = 0; i < 5; i++)
      lastFour[i] = fileName[nameLength - 4 + i]; // create a string with only the last four chars
    if (strcmp(ext, lastFour) == 0)
      return true;
    else
      return false;
  }
}

void savePatch()
{
  file.open(fileName, O_RDWR | O_CREAT); // create file if it doesn't exist and open the file for write
  // OSC1
  patchBuffer[0] = osc1WaveType;
  patchBuffer[1] = osc1Octave;
  patchBuffer[2] = osc1Volume;
  patchBuffer[3] = osc1Detune;
  patchBuffer[4] = pulseWidth;
  // OSC2
  patchBuffer[5] = osc2WaveType;
  patchBuffer[6] = osc2Octave;
  patchBuffer[7] = osc2Volume;
  patchBuffer[8] = osc2Detune;
  // FILTER
  patchBuffer[9] = filterCutoff;
  patchBuffer[10] = filterResonance;
  patchBuffer[11] = fType;
  patchBuffer[12] = filterBypass;
  // ENVELOPE
  patchBuffer[13] = attackTime;
  patchBuffer[14] = decayTime;
  patchBuffer[15] = sustainLevel;
  patchBuffer[16] = releaseTime;
  // LFO
  patchBuffer[17] = lfoShape;
  patchBuffer[18] = lfoRate;
  patchBuffer[19] = lfoLowRange;
  patchBuffer[20] = retrigger;
  // MODULATION
  patchBuffer[21] = lfoOsc1DetuneFactor;
  patchBuffer[22] = lfoOsc2DetuneFactor;
  patchBuffer[23] = osc1OctaveMod;
  patchBuffer[24] = osc2OctaveMod;
  patchBuffer[25] = lfoAmount;
  patchBuffer[26] = lfoAmpFactor;
  patchBuffer[27] = lfoPwFactor;
  patchBuffer[28] = envOsc1PitchFactor;
  patchBuffer[29] = envOsc2PitchFactor;
  patchBuffer[30] = envFilterCutoffFactor;
  patchBuffer[31] = envLfoRate;
  // SHAPER & GAIN
  patchBuffer[32] = shaperType;
  patchBuffer[33] = shaperType1PotVal;
  patchBuffer[34] = waveShapeAmount2;
  patchBuffer[35] = gainAmountPotVal;
  patchBuffer[36] = bitMuncher;
  // MONO & PORTA
  patchBuffer[37] = portamento;
  patchBuffer[38] = monoMode;
  patchBuffer[39] = unison;
  patchBuffer[40] = uniSpread;
  // write zeros to 100 for future use
  for (byte i = 41; i < 100; i++)
    patchBuffer[i] = 0;
  // USER 1 WAVESHAPE
  for (uint16_t i = 0; i < 600; i++)
    patchBuffer[i + 100] = nUserTable1[i];
  // USER 2 WAVESHAPE
  for (uint16_t i = 0; i < 600; i++)
    patchBuffer[i + 700] = nUserTable2[i];
  // USER 3 WAVESHAPE
  for (uint16_t i = 0; i < 600; i++)
    patchBuffer[i + 1300] = nUserTable3[i];
  // WRITE TO SD
  if (file.write(patchBuffer, 7600) != -1) // note - we are writing 1900 4 byte ints from the patch buffer to 7600 bytes on the SD
  {
    if (file.sync())
    {
      lcd.setCursor(4, 1);
      lcd.print("Saved!      ");
    }
  }
  file.close();
}

void loadPatch()
{
  loadRampUp = false;
  loadRampDown = true;
}

void loadProceed()
{
  file.open(fileName);
  if (file.read(patchBuffer, 7600) == 7600) // note - we are reading 7600 bytes to a buffer of 1900 4 byte ints
  {
    // OSC1
    osc1WaveType = patchBuffer[0];
    setOsc1WaveType(osc1WaveType);
    osc1Octave = patchBuffer[1];
    osc1Volume = patchBuffer[2];
    osc1Detune = patchBuffer[3];
    pulseWidth = patchBuffer[4];
    uiPulseWidth = pulseWidth;
    // OSC2
    osc2WaveType = patchBuffer[5];
    setOsc2WaveType(osc2WaveType);
    osc2Octave = patchBuffer[6];
    osc2Volume = patchBuffer[7];
    osc2Detune = patchBuffer[8];
    // FILTER
    filterCutoff = patchBuffer[9];
    filterResonance = patchBuffer[10];
    setFilterResonance(filterResonance);
    fType = patchBuffer[11];
    filterBypass = patchBuffer[12];
    // ENVELOPE
    attackTime = patchBuffer[13];
    decayTime = patchBuffer[14];
    sustainLevel = patchBuffer[15];
    releaseTime = patchBuffer[16];
    // LFO
    lfoShape = patchBuffer[17];
    setLfoShape(lfoShape);
    lfoRate = patchBuffer[18];
    userLfoRate = lfoRate;
    lfoLowRange = patchBuffer[19];
    retrigger = patchBuffer[20];
    // MODULATION
    lfoOsc1DetuneFactor = patchBuffer[21];
    lfoOsc2DetuneFactor = patchBuffer[22];
    osc1OctaveMod = patchBuffer[23];
    osc2OctaveMod = patchBuffer[24];
    lfoAmount = patchBuffer[25];
    lfoAmpFactor = patchBuffer[26];
    lfoPwFactor = patchBuffer[27];
    envOsc1PitchFactor = patchBuffer[28];
    envOsc2PitchFactor = patchBuffer[29];
    envFilterCutoffFactor = patchBuffer[30];
    envLfoRate = patchBuffer[31];
    // USER 1 WAVESHAPE
    for (uint16_t i = 0; i < 600; i++)
      nUserTable1[i] = patchBuffer[i + 100];
    // USER 2 WAVESHAPE
    for (uint16_t i = 0; i < 600; i++)
      nUserTable2[i] = patchBuffer[i + 700];
    // USER 3 WAVESHAPE
    for (uint16_t i = 0; i < 600; i++)
      nUserTable3[i] = patchBuffer[i + 1300];
    // SHAPER & GAIN
    int changeCounter = 0;
    if (patchBuffer[32] != shaperType)
    {
      shaperType = patchBuffer[32];
      changeCounter++;
    }
    if (patchBuffer[33] != shaperType1PotVal)
    {
      shaperType1PotVal = patchBuffer[33];
      float tmp = (float)shaperType1PotVal / 1024;
      waveShapeAmount = tmp;
      changeCounter++;
    }
    if (patchBuffer[34] != waveShapeAmount2)
    {
      waveShapeAmount2 = patchBuffer[34];
      changeCounter++;
    }
    if (changeCounter) // ie. changeCounter is not 0
      createWaveShaper();
    if (patchBuffer[35] != gainAmountPotVal)
    {
      gainAmountPotVal = patchBuffer[35];
      float tmp = (float)gainAmountPotVal / 1024;
      tmp += 1.0;
      gainAmount = tmp;
      createGainTable();
    }
    bitMuncher = patchBuffer[36];
    // MONO & PORTA
    portamento = patchBuffer[37];
    monoMode = patchBuffer[38];
    unison = patchBuffer[39];
    uniSpread = patchBuffer[40];
    if (uniSpread == 0)
      uniSpread = 10000;
  }
  file.close();
  synthPatchLoaded = true;
  loadRampUp = true;
}

void setFolder()
{
  dirCount = 0;
  dirChecked = false;
  switch (inFolder)
  {
  case true:
    inFolder = false;
    sd.chdir(); // change current directory to root
    sd.vwd()->rewind();
    clearJust();
    valueChange = true;
    break;

  case false:
    inFolder = true;
    sd.chdir(folderName); // change current directory to folderName
    sd.vwd()->rewind();
    clearJust();
    valueChange = true;
    break;
  }
}

void getSynthPatch()
{
  while (tempCount < dirChoice && (file.openNext(sd.vwd(), O_READ)))
  {
    tempCount++;
    file.getFilename(fileName);
    if (file.isDir())
    {
      folder = true;
      file.getFilename(folderName);
    }
    else
      folder = false;
    file.close();
  }
  if (tempCount == dirChoice)
  {
    lcd.setCursor(0, 1);
    lcd.print("    ");
    lcd.print(fileName);
    if (folder)
      arrow(strlen(fileName) + 4, 1);
    lcd.print("            "); // sloppy way to clear excess characters
  }
  if (!synthPatchLoaded)
  {
    if (checkExtension(".TB2"))
      loadPatch();
  }
  //  }
}

void saveSynthPatch()
{
  if (dirCount == 0 && dirChecked == false)
  {
    static uint16_t tempCount = 0;
    while (file.openNext(sd.vwd(), O_READ))
    {
      file.close();
      dirCount++;
    }
    dirChoice = 0;
    sd.vwd()->rewind();
    dirChecked = true;
    valueChange = true;
    if (dirCount < 98)
    {
      numberName = dirCount + 1;
      if (numberName < 10)
      {
        sprintf(saveName, "0%d.TB2", numberName);
      }
      else
      {
        sprintf(saveName, "%d.TB2", numberName);
      }
      lcd.setCursor(0, 1);
      lcd.print("    ");
      lcd.print(saveName);
      lcd.print("      ");
      strcpy(fileName, saveName);
    }
    else
    {
      lcd.setCursor(0, 1);
      lcd.print("    Folder Full!");
    }
  }

  else
  {
    while (tempCount < dirChoice)
    {
      file.openNext(sd.vwd(), O_READ);
      tempCount++;
      file.getFilename(fileName);
      if (file.isDir())
      {
        folder = true;
        file.getFilename(folderName);
      }
      else
        folder = false;
      file.close();
    }
    if (tempCount == dirChoice)
    {
      lcd.setCursor(0, 1);
      lcd.print("    ");
      lcd.print(fileName);
      if (folder)
        arrow(strlen(fileName) + 4, 1);
      lcd.print("            "); // sloppy way to clear excess characters
    }
  }
}

void getFirstFile()
{
  sd.vwd()->rewind();
  file.openNext(sd.vwd(), O_READ);
  file.getFilename(fileName);
  if (file.isDir())
  {
    folder = true;
    file.getFilename(folderName);
  }
  file.close();
}

void prepNextChoice()
{
  sd.vwd()->rewind();
  tempCount = 0;
  waveshapeLoaded = false;
  synthPatchLoaded = false;
  seqBankLoaded = false;
}

void saveSeqBank()
{
  if (dirCount == 0 && dirChecked == false)
  {
    static uint16_t tempCount = 0;
    while (file.openNext(sd.vwd(), O_READ))
    {
      file.close();
      dirCount++;
    }
    dirChoice = 0;
    sd.vwd()->rewind();
    dirChecked = true;
    valueChange = true;
    if (dirCount < 98)
    {
      numberName = dirCount + 1;
      if (numberName < 10)
      {
        sprintf(saveName, "0%d.SEQ", numberName);
      }
      else
      {
        sprintf(saveName, "%d.SEQ", numberName);
      }
      lcd.setCursor(0, 1);
      lcd.print("    ");
      lcd.print(saveName);
      lcd.print("      ");
      strcpy(fileName, saveName);
    }
    else
    {
      lcd.setCursor(0, 1);
      lcd.print("    Folder Full!");
    }
  }

  else
  {
    while (tempCount < dirChoice)
    {
      file.openNext(sd.vwd(), O_READ);
      tempCount++;
      file.getFilename(fileName);
      if (file.isDir())
      {
        folder = true;
        file.getFilename(folderName);
      }
      else
        folder = false;
      file.close();
    }
    if (tempCount == dirChoice)
    {
      lcd.setCursor(0, 1);
      lcd.print("    ");
      lcd.print(fileName);
      if (folder)
        arrow(strlen(fileName) + 4, 1);
      lcd.print("            "); // sloppy way to clear excess characters
    }
  }
}

void saveBank()
{
  file.open(fileName, O_RDWR | O_CREAT); // create file if it doesn't exist and open the file for write
  // PACK THE BUFFER WITH CURRENT SEQUENCES
  for (byte i = 0; i < 8; i++) // for each of the 8 sequences
  {
    for (byte j = 0; j < 4; j++) // for each of the 4 voices
    {
      for (byte k = 0; k < 16; k++) // NOTE VALUES for each of the 16 steps
        seqBankBuffer[(i * 186) + (j * 16) + k] = seq[i].voice[j][k];
    }
    for (byte m = 0; m < 16; m++) // TIES for each of the 16 steps
      seqBankBuffer[(i * 186) + 64 + m] = seq[i].tie[m];
    for (byte n = 0; n < 16; n++) // MUTES for each of the 16 steps
      seqBankBuffer[(i * 186) + 80 + n] = seq[i].mute[n];
    for (byte o = 0; o < 16; o++) // VELOCITY for each of the 16 steps
      seqBankBuffer[(i * 186) + 96 + o] = seq[i].velocity[o];
    for (byte p = 0; p < 4; p++) // CC NUMBER for each of the 4 controllers
      seqBankBuffer[(i * 186) + 112 + p] = seq[i].controlNum[p];
    for (byte q = 0; q < 4; q++) // for each of the 4 controllers
    {
      for (byte r = 0; r < 16; r++) // CC VALUE for each of the 16 steps
        seqBankBuffer[(i * 186) + 116 + (q * 16) + r] = seq[i].controlValue[q][r];
    }
    seqBankBuffer[(i * 186) + 180] = seq[i].noteDur;       // NOTE DURATION
    seqBankBuffer[(i * 186) + 181] = seq[i].divSelection;  // TEMPO DIVISION
    seqBankBuffer[(i * 186) + 182] = seq[i].patternLength; // PATTERN LENGTH
    seqBankBuffer[(i * 186) + 183] = seq[i].transpose;     // TRANSPOSE VALUE
    seqBankBuffer[(i * 186) + 184] = seq[i].swing;         // SWING AMOUNT
    seqBankBuffer[(i * 186) + 185] = seq[i].bpm;           // TEMPO
  }
  seqBankBuffer[1488] = bankMode;
  for (int s = 1489; s < 1600; s++)
    seqBankBuffer[s] = 0; // write zeros for padding in case we need it for future use
  // WRITE TO SD
  if (file.write(seqBankBuffer, 6400) != -1) // note - we are writing 1600 4 byte ints from the patch buffer to 6400 bytes on the SD
  {
    if (file.sync())
    {
      lcd.setCursor(4, 1);
      lcd.print("Saved!      ");
    }
  }
  file.close();
}

void loadBank()
{
  file.open(fileName);
  if (file.read(seqBankBuffer, 6400) == 6400) // note - we are reading 6400 bytes to a buffer of 1600 4 byte ints
  {
    // we update the buffer with the sequence
    // but we only want to unpack it if we hit the enter key with unpackSeqBankBuffer()
  }
  file.close();
  seqBankLoaded = true;
}

void getSeqBank()
{
  if (dirCount == 0 && dirChecked == false)
  {
    static uint16_t tempCount = 0;
    while (file.openNext(sd.vwd(), O_READ))
    {
      file.close();
      dirCount++;
    }
    dirChoice = 0;
    sd.vwd()->rewind();
    dirChecked = true;
    valueChange = true;
  }

  else
  {
    while (tempCount < dirChoice)
    {
      file.openNext(sd.vwd(), O_READ);
      tempCount++;
      file.getFilename(fileName);
      if (file.isDir())
      {
        folder = true;
        file.getFilename(folderName);
      }
      else
        folder = false;
      file.close();
    }
    if (tempCount == dirChoice)
    {
      lcd.setCursor(0, 1);
      lcd.print("    ");
      lcd.print(fileName);
      if (folder)
        arrow(strlen(fileName) + 4, 1);
      lcd.print("            "); // sloppy way to clear excess characters
    }
    if (!seqBankLoaded)
    {
      if (checkExtension(".SEQ"))
        loadBank();
    }
  }
}

void unpackSeqBankBuffer()
{
  // UNPACK THE BUFFER TO CURRENT SEQUENCES
  for (byte i = 0; i < 8; i++) // for each of the 8 sequences
  {
    for (byte j = 0; j < 4; j++) // for each of the 4 voices
    {
      for (byte k = 0; k < 16; k++) // NOTE VALUES for each of the 16 steps
        seq[i].voice[j][k] = seqBankBuffer[(i * 186) + (j * 16) + k];
    }
    for (byte m = 0; m < 16; m++) // TIES for each of the 16 steps
      seq[i].tie[m] = seqBankBuffer[(i * 186) + 64 + m];
    for (byte n = 0; n < 16; n++) // MUTES for each of the 16 steps
      seq[i].mute[n] = seqBankBuffer[(i * 186) + 80 + n];
    for (byte o = 0; o < 16; o++) // VELOCITY for each of the 16 steps
      seq[i].velocity[o] = seqBankBuffer[(i * 186) + 96 + o];
    for (byte p = 0; p < 4; p++) // CC NUMBER for each of the 4 controllers
      seq[i].controlNum[p] = seqBankBuffer[(i * 186) + 112 + p];
    for (byte q = 0; q < 4; q++) // for each of the 4 controllers
    {
      for (byte r = 0; r < 16; r++) // CC VALUE for each of the 16 steps
        seq[i].controlValue[q][r] = seqBankBuffer[(i * 186) + 116 + (q * 16) + r];
    }
    seq[i].noteDur = seqBankBuffer[(i * 186) + 180];       // NOTE DURATION
    seq[i].divSelection = seqBankBuffer[(i * 186) + 181];  // TEMPO DIVISION
    seq[i].patternLength = seqBankBuffer[(i * 186) + 182]; // PATTERN LENGTH
    if (seqStep >= seq[i].patternLength)
      seqStep = 0;
    seq[i].transpose = seqBankBuffer[(i * 186) + 183]; // TRANSPOSE VALUE
    seq[i].swing = seqBankBuffer[(i * 186) + 184];     // SWING AMOUNT
    seq[i].bpm = seqBankBuffer[(i * 186) + 185];       // TEMPO
  }
  bankMode = seqBankBuffer[1488];
}

void loadSettings()
{
  sd.chdir(); // change current directory to root
  // if we're in the root and we can't open the file, make it with savePreferences()
  if (!file.open("TB2PREFS.set"))
  {
    saveSettings();
  }
  else // load the preferences
  {
    if (file.read(settingsBuffer, 400) == 400) // note - we are reading 400 bytes to a buffer of 100 4 byte ints
    {
      // UNPACK THE BUFFER TO SETTINGS
      midiOut = settingsBuffer[0];
      midiChannel = settingsBuffer[2];
      midiThruType = settingsBuffer[3];
      checkThru();
      midiSync = settingsBuffer[4];
      setSyncType();
      keysOut = settingsBuffer[5];
      keyVelocity = settingsBuffer[6];
      //settingsBuffer[7]
      //settingsBuffer[8]
      midiTriggerOut = settingsBuffer[9];
      midiTriggerChannel = settingsBuffer[10];
      if (midiTriggerChannel == 0)
        midiTriggerChannel = 1; // in case preferences have not yet been saved
      for (int i = 0; i < 8; i++)
        midiTrigger[i] = settingsBuffer[11 + i];
      volume = settingsBuffer[19];
      if (volume == 0)
        volume = 1023; // in case preferences have not yet been saved
      else if (volume == 1025)
        volume = 0;
    }
    file.close();
  }
}

void saveSettings()
{

  // PACK THE BUFFER WITH SEETTINGS
  for (int i = 0; i < 100; i++)
    settingsBuffer[i] = 0; // clear the buffer

  settingsBuffer[0] = midiOut;
  settingsBuffer[2] = midiChannel;
  settingsBuffer[3] = midiThruType;
  settingsBuffer[4] = midiSync;
  settingsBuffer[5] = keysOut;
  settingsBuffer[6] = keyVelocity;
  //settingsBuffer[7]
  //settingsBuffer[8]
  settingsBuffer[9] = midiTriggerOut;
  settingsBuffer[10] = midiTriggerChannel;
  for (int i = 0; i < 8; i++)
    settingsBuffer[11 + i] = midiTrigger[i];
  settingsBuffer[19] = (volume > 0) ? volume : 1025;
  file.open("TB2PREFS.set", O_RDWR | O_CREAT); // create file if it doesn't exist and open the file for write
  if (file.write(settingsBuffer, 400) != -1)   // note - we are writing 100 4 byte ints from the patch buffer to 400 bytes on the SD
  {
    if (file.sync())
    {
      lcd.setCursor(0, 1);
      lcd.print("Settings Saved! ");
    }
  }
  file.close();
}

void gotoRootDir()
{
  inFolder = false;
  sd.chdir(); // change current directory to root
  sd.vwd()->rewind();
}

// SEQUENCER.ino
void seqNextStep()
{
  if (seqStep == 0 && currentSeq != selectedSeq) // to switch to the next selected sequence
  {
    currentSeq = selectedSeq;
    seqUpdateDisplay = true;
    cueNextSeq();
  }

  switch (seqPlayMode) // sets the seqIncrement variable (how many steps the sequence is advance by) according to the play mode
  {
  case 0: // forward
    seqIncrement = 1;
    break;

  case 1: // reverse
    seqIncrement = -1;
    break;

  case 2: // pendulum
    if (seqStep == 0)
      seqIncrement = 1;
    else if (seqStep == seq[currentSeq].patternLength - 1)
      seqIncrement = -1;
    break;

  case 3:             // random interval
    if (seqStep == 0) // choose another interval when the step reaches 0
      seqIncrement = random(1, seq[currentSeq].patternLength);
    break;

  case 4: // drunk
  {
    byte decider = random(0, 2);
    if (decider == 0)
      seqIncrement = 1;
    else
      seqIncrement = -1;
  }
  break;

  case 5: // random
    seqIncrement = random(1, seq[currentSeq].patternLength);
    break;
  }

  if (!seq[currentSeq].mute[seqStep] && !seq[currentSeq].tie[seqStep]) // if this step isn't muted or a tie step
  {
    if (!monoMode)
    {
      for (int i = 0; i < 4; i++)
      {
        voice[i] = (seq[currentSeq].voice[i][seqStep] != 255) ? seq[currentSeq].voice[i][seqStep] + seq[currentSeq].transpose : 255;
        seqMidiOn[i] = voice[i];
        if (voice[i] == 255)
          muteVoice[i] = true;
      }
    }
    else // mono mode
    {
      voice[0] = (seq[currentSeq].voice[0][seqStep] != 255) ? seq[currentSeq].voice[0][seqStep] + seq[currentSeq].transpose : 255;
      seqMidiOn[0] = voice[0];
      if (voice[0] == 255)
        muteVoice[0] = true;
      else
      {
        for (byte j = 1; j < 4; j++)
        {
          if (j - 1 < unison)
            voice[j] = voice[0];
          else
          {
            voice[j] = 255;
            muteVoice[j] = true;
          }
        }
      }
    }
    if (!seq[currentSeq].tie[nextStep()])
      seqReleasePulse = (pulseCounter + map(seq[currentSeq].noteDur, 0, 1023, 4, (seqDivision[seq[currentSeq].divSelection]) - 1)) % 96;

    if (seqMidiOn[0] != 255)
    {
      noteTrigger();
      setVeloModulation(seq[currentSeq].velocity[seqStep]);
      outVelocity = seq[currentSeq].velocity[seqStep];
      seqReleased = false;
    }
    seqSendMidiNoteOns = true; // trigger the sending of notes in the loop (can't do it here in the interrupt)
  }

  if (seq[currentSeq].tie[seqStep] && !seq[currentSeq].tie[nextStep()])
    seqReleasePulse = (pulseCounter + map(seq[currentSeq].noteDur, 0, 1023, 4, (seqDivision[seq[currentSeq].divSelection]) - 1)) % 96;

  if (seq[currentSeq].mute[seqStep])
  {
    if (seqReleasePulse != 255)
      seqReleasePulse = (pulseCounter + 1) % 96;
  }

  seqStep = nextStep();
  longStep = !longStep;
}

int nextStep()
{
  // deal with seqIncrement that's out of range
  if (seqIncrement == 0) // we don't want to deal with a zero
    seqIncrement = 1;

  int followingStep = seqStep + seqIncrement;
  if (followingStep >= seq[currentSeq].patternLength)
    followingStep %= seq[currentSeq].patternLength;
  else if (followingStep < 0)
    followingStep += seq[currentSeq].patternLength;
  return followingStep;
}

void copySeq() // copy the current sequence to another location
{
  for (byte j = 0; j < 4; j++) // for each of the 4 voices
  {
    for (byte k = 0; k < 16; k++) // NOTE VALUES for each of the 16 steps
      seq[destinationSeq].voice[j][k] = seq[sourceSeq].voice[j][k];
  }
  for (byte m = 0; m < 16; m++) // TIES for each of the 16 steps
    seq[destinationSeq].tie[m] = seq[sourceSeq].tie[m];
  for (byte n = 0; n < 16; n++) // MUTES for each of the 16 steps
    seq[destinationSeq].mute[n] = seq[sourceSeq].mute[n];
  for (byte o = 0; o < 16; o++) // VELOCITY for each of the 16 steps
    seq[destinationSeq].velocity[o] = seq[sourceSeq].velocity[o];
  for (byte p = 0; p < 4; p++) // CC NUMBER for each of the 4 controllers
    seq[destinationSeq].controlNum[p] = seq[sourceSeq].controlNum[p];
  for (byte q = 0; q < 4; q++) // for each of the 4 controllers
  {
    for (byte r = 0; r < 16; r++) // CC VALUE for each of the 16 steps
      seq[destinationSeq].controlValue[q][r] = seq[sourceSeq].controlValue[q][r];
  }
  seq[destinationSeq].noteDur = seq[sourceSeq].noteDur;             // NOTE DURATION
  seq[destinationSeq].divSelection = seq[sourceSeq].divSelection;   // TEMPO DIVISION
  seq[destinationSeq].patternLength = seq[sourceSeq].patternLength; // PATTERN LENGTH
  seq[destinationSeq].transpose = seq[sourceSeq].transpose;         // TRANSPOSE VALUE
  seq[destinationSeq].swing = seq[sourceSeq].swing;                 // SWING AMOUNT
  seq[destinationSeq].bpm = seq[sourceSeq].bpm;                     // TEMPO
  copied = true;
}

void clearSeq()
{
  noteRelease();
  for (byte j = 0; j < 4; j++) // for each of the 4 voices
  {
    for (byte k = 0; k < 16; k++) // NOTE VALUES for each of the 16 steps
      seq[currentSeq].voice[j][k] = 255;
  }
  for (byte m = 0; m < 16; m++) // TIES for each of the 16 steps
    seq[currentSeq].tie[m] = 0;
  for (byte n = 0; n < 16; n++) // MUTES for each of the 16 steps
    seq[currentSeq].mute[n] = 0;
  for (byte o = 0; o < 16; o++) // VELOCITY for each of the 16 steps
    seq[currentSeq].velocity[o] = 255;
  for (byte p = 0; p < 4; p++) // CC NUMBER for each of the 4 controllers
    seq[currentSeq].controlNum[p] = 255;
  for (byte q = 0; q < 4; q++) // for each of the 4 controllers
  {
    for (byte r = 0; r < 16; r++) // CC VALUE for each of the 16 steps
      seq[currentSeq].controlValue[q][r] = 255;
  }
  seq[currentSeq].noteDur = 1023;     // NOTE DURATION
  seq[currentSeq].divSelection = 1;   // TEMPO DIVISION
  seq[currentSeq].patternLength = 16; // PATTERN LENGTH
  seq[currentSeq].transpose = 0;      // TRANSPOSE VALUE
  seq[currentSeq].swing = 0;          // SWING AMOUNT
  seq[currentSeq].bpm = 120;          // TEMPO
}
void seqPlayStop()
{
  seqRunning = !seqRunning;
  seqStep = 0; // rewind the sequence
  pulseCounter = 0;
  eighthCounter = 0;
  lfo8thSyncCounter = 0;
  longStep = true;
  if (seqRunning)
  {
    cueNextSeq();
    seqJustStarted = true;
    if (midiClockOut)
      midiA.sendRealTime(midi::Start); // send a midi clock start signal
  }
  if (!seqRunning)
  {
    seqSendMidiNoteOffs = true;
    noteRelease();
    if (midiClockOut)
      midiA.sendRealTime(midi::Stop); // send a midi clock start signal
  }
}

void cueNextSeq()
{
  switch (bankMode)
  {
  case 0: // do nothing
    break;
  case 1:                      // loop 2
    if ((currentSeq % 2) == 0) // for even numbered sequences
      selectedSeq = currentSeq + 1;
    else
      selectedSeq = currentSeq - 1;
    break;
  case 2: // loop 4
    if (currentSeq < 3)
      selectedSeq = currentSeq + 1;
    else if (currentSeq == 3)
      selectedSeq = 0;
    else if (currentSeq < 7)
      selectedSeq = currentSeq + 1;
    else if (currentSeq == 7)
      selectedSeq = 4;
    break;
  case 3:
    if (currentSeq < 7)
      selectedSeq = currentSeq + 1;
    else
      selectedSeq = 0;
    break;
  case 4:
    do
      selectedSeq = random(0, 8);      // upper bound is not included in random()
    while (selectedSeq == currentSeq); // if the random function delivers the currentSeq we'll get stuck
    break;
  }
}

boolean updateSeqNotes()
{
  assignIncrementButtons(&seqEditStep, 0, 15, 1);
  if (!midiMode)
  {
    for (byte i = 0; i < 4; i++)
      seq[currentSeq].voice[i][seqEditStep] = 255;
    byte useVoice = 0;
    for (byte i = 0; i < 13; i++)
    {
      if (pressed[i] && useVoice < 4)
      {
        seq[currentSeq].voice[useVoice][seqEditStep] = i;
        useVoice++;
      }
    }
    seq[currentSeq].velocity[seqEditStep] = keyVelocity;
    seq[currentSeq].mute[seqEditStep] = 0;
    seq[currentSeq].tie[seqEditStep] = 0;
    valueChange = true;
    return true;
  }
  else // MIDI mode
  {
    byte voxCounter = 0;
    for (byte i = 0; i < 4; i++)
    {
      seq[currentSeq].voice[i][seqEditStep] = 255;

      if (voice[i] != 255)
      {
        seq[currentSeq].voice[voxCounter][seqEditStep] = voice[i];
        voxCounter++;
      }
    }
    if (voxCounter != 0)
    {
      seq[currentSeq].velocity[seqEditStep] = midiVelocity;
      seq[currentSeq].mute[seqEditStep] = 0;
      seq[currentSeq].tie[seqEditStep] = 0;
      valueChange = true;
      return true;
    }
    else
      return false;
  }
}

void clearStep()
{
  for (byte j = 0; j < 4; j++) // NOTE for each of the 4 voices
    seq[currentSeq].voice[j][seqEditStep] = 255;
  // TIE
  seq[currentSeq].tie[seqEditStep] = 0;
  // MUTE
  seq[currentSeq].mute[seqEditStep] = 0;
  // VELOCITY
  seq[currentSeq].velocity[seqEditStep] = 255;
  // CONTROLLERS
  for (byte q = 0; q < 4; q++) // for each of the 4 controllers
    seq[currentSeq].controlValue[q][seqEditStep] = 255;
}

// SYNTH.ino

// *** WAVE SHAPES ***

// fill the note table with the phase increment values we require to generate the note
void createNoteTable(float fSampleRate)
{
  for (uint32_t unMidiNote = 0; unMidiNote < MIDI_NOTES; unMidiNote++)
  {
    float fFrequency = ((pow(2.0, (unMidiNote - 69.0) / 12.0)) * 440.0);
    nMidiPhaseIncrement[unMidiNote] = fFrequency * TICKS_PER_CYCLE;
  }
}

// create the individual samples for our sinewave table
void createSineTable()
{
  for (uint32_t nIndex = 0; nIndex < WAVE_SAMPLES; nIndex++)
  {
    // SINE
    nSineTable[nIndex] = (uint16_t)(((1 + sin(((2.0 * PI) / WAVE_SAMPLES) * nIndex)) * 4095.0) / 2);
  }
}

void createSquareTable(int16_t pw)
{
  static int16_t lastPw = 127; // don't initialize to 0
  if (pw != lastPw)
  {
    for (uint32_t nIndex = 0; nIndex < WAVE_SAMPLES; nIndex++)
    {
      // SQUARE
      if (nIndex <= ((WAVE_SAMPLES / 2) + pw))
        nSquareTable[nIndex] = 0;
      else
        nSquareTable[nIndex] = 4095;
    }
    lastPw = pw;
  }
}

void createSawTable()
{
  for (uint32_t nIndex = 0; nIndex < WAVE_SAMPLES; nIndex++)
  {
    // SAW
    nSawTable[nIndex] = (4095 / WAVE_SAMPLES) * nIndex;
  }
}

void createTriangleTable()
{
  for (uint32_t nIndex = 0; nIndex < WAVE_SAMPLES; nIndex++)
  {
    // Triangle
    if (nIndex < WAVE_SAMPLES / 2)
      nTriangleTable[nIndex] = (4095 / (WAVE_SAMPLES / 2)) * nIndex;
    else
      nTriangleTable[nIndex] = (4095 / (WAVE_SAMPLES / 2)) * (WAVE_SAMPLES - nIndex);
  }
}

void clearUserTables()
{
  for (uint32_t nIndex = 0; nIndex < WAVE_SAMPLES; nIndex++)
  {
    nUserTable1[nIndex] = 2048; // 2048 is silence
    nUserTable2[nIndex] = 2048;
    nUserTable3[nIndex] = 2048;
  }
}

// *** SYNTH ENGINE ***

void audioHandler()
{
  for (int i = 0; i < 8; i++)
  {
    ulPhaseAccumulator[i] += ulPhaseIncrement[i]; // 32 bit phase increment, see below
  }

  // if the phase accumulator over flows - we have been through one cycle at the current pitch,
  // now we need to reset the grains ready for our next cycle
  for (int i = 0; i < 8; i++)
  {
    if (ulPhaseAccumulator[i] > SAMPLES_PER_CYCLE_FIXEDPOINT)
    {
      // DB 02/Jan/2012 - carry the remainder of the phase accumulator
      ulPhaseAccumulator[i] -= SAMPLES_PER_CYCLE_FIXEDPOINT;
    }
  }

  int32_t ulOutput[8];

  // get the current sample
  for (int i = 0; i < 8; i++)
  {
    switch (voiceSounding[i % 4])
    {
    case true:
      ulOutput[i] = *(wavePointer[i] + (ulPhaseAccumulator[i] >> 20));
      break;
    case false:
      ulOutput[i] = 2048; // 2048 is silence
      break;
    }
  }

  int32_t sampleOsc1;
  int32_t sampleOsc2;

  for (int i = 0; i < 4; i++)
  {
    sampleOsc1 += ulOutput[i];
  }
  sampleOsc1 = sampleOsc1 / 4;

  for (int i = 0; i < 4; i++)
  {
    sampleOsc2 += ulOutput[i + 4];
  }
  sampleOsc2 = sampleOsc2 / 4;

  // look up the volume for the current sample
  if (osc1WaveType == 7)                        // noise
    sampleOsc1 = osc1VolTable[random(0, 4096)]; //(((random(0, 4096) - 2048) * osc1Volume) >> 10) + 2048;
  else
    sampleOsc1 = osc1VolTable[sampleOsc1]; //(((sampleOsc1 - 2048) * osc1Volume) >> 10) + 2048;

  if (osc2WaveType == 7)                        // noise
    sampleOsc2 = osc2VolTable[random(0, 4096)]; //(((random(0, 4096) - 2048) * osc2Volume) >> 10) + 2048;
  else
    sampleOsc2 = osc2VolTable[sampleOsc2]; //(((sampleOsc2 - 2048) * osc2Volume) >> 10) + 2048;

  // get the panning for the two oscillators
  //int32_t sampleOutL = (((((sampleOsc1 - 2048) * (osc1PanL)) >> 10) + 2048) + ((((sampleOsc2 - 2048) * (osc2PanL)) >> 10) + 2048)) >> 1;
  //int32_t sampleOutR = (((((sampleOsc1 - 2048) * osc1PanR) >> 10) + 2048) + ((((sampleOsc2 - 2048) * osc2PanR) >> 10) + 2048)) >> 1;

  // mix the comboOscillators to one channel
  int32_t sampleMix = (sampleOsc1 + sampleOsc2) >> 1;

  // XOR mix
  // int32_t sampleMix = sampleOsc1 ^ sampleOsc2;

  // bitMucher
  int32_t bitMuncherOut = (((sampleMix - 2048) >> bitMuncher) << bitMuncher) + 2048;

  // get the current envelope volumes
  int32_t envelopeOut = (((bitMuncherOut - 2048) * envelopeVolume) >> 10) + 2048;
  //int32_t envelopeOutR = (((filterOutR - 2048) * envelopeVolume) >> 10) + 2048;

  // get the LFO volume
  int32_t lfoAmpOut = (((envelopeOut - 2048) * lfoAmp) >> 10) + 2048;

  // get the velocity volume
  int32_t velAmpOut = (((lfoAmpOut - 2048) * (1023 - velAmp)) >> 10) + 2048;

  // waveshaper
  int32_t waveShaperOut = waveShaper[velAmpOut];

  // get the filter
  int32_t filterOut = filterNextL(waveShaperOut);
  //int32_t filterOutL = filterNextL(sampleOutL);
  //int32_t filterOutR = filterNextR(sampleOutR);;

  // constrain the filter output
  if (filterOut > 4095)
    filterOut = 4095;
  //if (filterOutR > 4095) filterOutR = 4095;

  if (filterOut < 0)
    filterOut = 0;
  // if (filterOutR < 0) filterOutR = 0;

  // gain
  int32_t gainOut = gainTable[filterOut];

  // load mute - needed so we don't get load thunks when loading patches
  int32_t loadMuteOut = (((gainOut - 2048) * loadRampFactor) >> 10) + 2048;

  // volume
  int32_t volumeOut = (((loadMuteOut - 2048) * volume) >> 10) + 2048;

  // write to DAC0
  dacc_set_channel_selection(DACC_INTERFACE, 0);
  dacc_write_conversion_data(DACC_INTERFACE, volumeOut);
  // write to DAC1
  dacc_set_channel_selection(DACC_INTERFACE, 1);
  dacc_write_conversion_data(DACC_INTERFACE, volumeOut);
}

void assignVoices()
{
  static uint32_t incrementSource[8];
  static uint32_t incrementCurrent[8];
  static uint32_t incrementTarget[8];

  for (int i = 0; i < 8; i++)
  {
    if (voice[i % 4] != 255)
    {
      voiceSounding[i % 4] = true;
      if (i < 4)
      {
        incrementTarget[i] = nMidiPhaseIncrement[voice[i % 4] + (osc1OctaveOut * 12) + osc1Detune];
      }
      else
      {
        incrementTarget[i] = nMidiPhaseIncrement[voice[i % 4] + (osc2OctaveOut * 12)] + osc2Detune;
      }
    }
  }

  if (portamento == 0 || millis() >= portaEndTime)
  {
    for (byte i = 0; i < 8; i++)
    {
      incrementCurrent[i] = incrementTarget[i];
      incrementSource[i] = incrementTarget[i];
    }
  }
  else
  {
    for (byte i = 0; i < 8; i++)
      incrementCurrent[i] = map(millis(), portaStartTime, portaEndTime, incrementSource[i], incrementTarget[i]);
  }

  if (monoMode && unison)
  {
    for (byte i = 0; i < 8; i++)
    {
      if (i < 4)
        incrementCurrent[i] += (uniSpread * i);
      else
        incrementCurrent[i] += (uniSpread * (i - 4));
    }
  }
  //MOD detune
  for (int i = 0; i < 8; i++)
  {
    if (i < 4)
      ulPhaseIncrement[i] = constrain(incrementCurrent[i] + lfoOsc1Detune + envOsc1Pitch + velOsc1Detune, 0, 178954880);
    else
      ulPhaseIncrement[i] = constrain(incrementCurrent[i] + lfoOsc2Detune + envOsc2Pitch + velOsc2Detune, 0, 178954880);
  }
}

void setOsc1WaveType(int shape)
{
  osc1WaveType = shape;
  for (int i = 0; i < 4; i++)
  {
    switch (shape)
    {
    case 0: // sine
      wavePointer[i] = &nSineTable[0];
      break;
    case 1: // triangle
      wavePointer[i] = &nTriangleTable[0];
      break;
    case 2: // saw
      wavePointer[i] = &nSawTable[0];
      break;
    case 3: // square
      wavePointer[i] = &nSquareTable[0];
      break;
    case 4: // user1
      wavePointer[i] = &nUserTable1[0];
      break;
    case 5: // user2
      wavePointer[i] = &nUserTable2[0];
      break;
    case 6: // user3
      wavePointer[i] = &nUserTable3[0];
      break;
    }
  }
}

void setOsc2WaveType(int shape)
{
  osc2WaveType = shape;
  for (int i = 0; i < 4; i++)
  {
    switch (shape)
    {
    case 0: // sine
      wavePointer[i + 4] = &nSineTable[0];
      break;
    case 1: // triangle
      wavePointer[i + 4] = &nTriangleTable[0];
      break;
    case 2: // saw
      wavePointer[i + 4] = &nSawTable[0];
      break;
    case 3: // square
      wavePointer[i + 4] = &nSquareTable[0];
      break;
    case 4: // user1
      wavePointer[i + 4] = &nUserTable1[0];
      break;
    case 5: // user2
      wavePointer[i + 4] = &nUserTable2[0];
      break;
    case 6: // user3
      wavePointer[i + 4] = &nUserTable3[0];
      break;
    }
  }
}

// lookup tables for OSC1 & OSC2 volume
void createOsc1Volume()
{
  for (int i = 0; i < 4096; i++)
    osc1VolTable[i] = (((i - 2048) * osc1Volume) >> 10) + 2048;
}

void createOsc2Volume()
{
  for (int i = 0; i < 4096; i++)
    osc2VolTable[i] = (((i - 2048) * osc2Volume) >> 10) + 2048;
}

void createGainTable()
{
  for (int i = 0; i < 4096; i++)
  {
    float tmp = (((float)i - 2048) * gainAmount) + 2048;
    // hard clip
    if (tmp > 4095)
      tmp = 4095;
    else if (tmp < 0)
      tmp = 0;
    gainTable[i] = (int)tmp;
  }
}

// UI.ino

// *** LED ***
void updateLED()
{
  if (lfoLED)
    analogWrite(LED, tmpLFO >> 4);
  else if (LEDon)
    analogWrite(LED, 255);
  else
    analogWrite(LED, 0);
}

// *** LCD ***
void showValue(byte h, byte v, int number) // a function to help us deal with displaying numbers of varying lengths
{
  if (number < 10)
  {
    lcd.setCursor(h, v);
    lcd.print(number);
    lcd.print("  ");
  }
  else if (number < 100)
  {
    lcd.setCursor(h, v);
    lcd.print(number);
    lcd.print(" ");
  }
  else
  {
    lcd.setCursor(h, v);
    lcd.print(number);
  }
}

// *** UI ***
void updateMenu()
{
  arrowAnimation = false;
  switch (menu)
  {
  case 0: // SPLASH/MAIN
    clearLCD();
    lcd.setCursor(0, 0);
    lcd.print("MAIN            ");
    switch (mainMenu)
    {
    case 0:
      lcd.setCursor(0, 1);
      lcd.print("Synth           ");
      lcd.setCursor(11, 0);
      lcd.print("Patch");
      if (synPatchLoadSave == 0) // load
      {
        lcd.setCursor(11, 1);
        lcd.print("Load      ");
      }
      else // save
      {
        lcd.setCursor(11, 1);
        lcd.print("Save      ");
      }
      arrow(15, 1);
      break;
    case 1:
      lcd.setCursor(0, 1);
      lcd.print("Arpeggiator     ");
      break;
    case 2:
      lcd.setCursor(0, 1);
      lcd.print("Sequencer       ");
      lcd.setCursor(11, 0);
      lcd.print("Bank ");
      if (seqBankLoadSave == 0) // load
      {
        lcd.setCursor(11, 1);
        lcd.print("Load      ");
      }
      else // save
      {
        lcd.setCursor(11, 1);
        lcd.print("Save      ");
      }
      arrow(15, 1);
      break;
    case 3: // SETTINGS
      lcd.setCursor(0, 1);
      lcd.print("Settings   Save ");
      if (!settingsConfirm)
        arrow(15, 1);
      break;
    }
    break;
  case 10:
    clearLCD();
    lcd.setCursor(0, 0);
    lcd.print("OSCILLATORS     ");
    lcd.setCursor(0, 1);
    lcd.print("OSC1            ");
    break;
  case 20:
    clearLCD();
    lcd.setCursor(0, 0);
    lcd.print("OSCILLATORS     ");
    lcd.setCursor(0, 1);
    lcd.print("OSC2            ");
    break;
  case 30:
    clearLCD();
    lcd.setCursor(0, 0);
    lcd.print("FILTER          ");
    break;
  case 40:
    clearLCD();
    lcd.setCursor(0, 0);
    lcd.print("ENVELOPE (ADSR) ");
    break;
  case 50:
    clearLCD();
    lcd.setCursor(0, 0);
    lcd.print("LFO             ");
    break;
  case 60:
    clearLCD();
    lcd.setCursor(0, 0);
    lcd.print("MODULATION      ");
    break;
  case 65:
    clearLCD();
    lcd.setCursor(0, 0);
    lcd.print("SHAPER & GAIN   ");
    break;
  case 68:
    clearLCD();
    lcd.setCursor(0, 0);
    lcd.print("MONO & PORTA    ");
    break;
  case 100:
    clearLCD();
    lcd.setCursor(0, 0);
    lcd.print("ARPEGGIATOR     ");
    lcd.setCursor(0, 1);
    lcd.print("Page 1          ");
    break;
  case 110:
    clearLCD();
    lcd.setCursor(0, 0);
    lcd.print("ARPEGGIATOR     ");
    lcd.setCursor(0, 1);
    lcd.print("Page 2          ");
    break;
  case 200:
    clearLCD();
    lcd.setCursor(0, 0);
    lcd.print("SEQUENCER       ");
    lcd.setCursor(0, 1);
    lcd.print("Trigger         ");
    break;
  case 210:
    clearLCD();
    lcd.setCursor(0, 0);
    lcd.print("SEQUENCER       ");
    lcd.setCursor(0, 1);
    lcd.print("Edit            ");
    break;
  case 220:
    clearLCD();
    lcd.setCursor(0, 0);
    lcd.print("SEQUENCER       ");
    lcd.setCursor(0, 1);
    lcd.print("Settings        ");
    break;
  case 230:
    clearLCD();
    lcd.setCursor(0, 0);
    lcd.print("SEQUENCER       ");
    lcd.setCursor(0, 1);
    lcd.print("Bank Settings   ");
    break;
  case 300:
    clearLCD();
    lcd.setCursor(0, 0);
    lcd.print("SETTINGS        ");
    lcd.setCursor(0, 1);
    lcd.print("MIDI            ");
    break;
  case 310:
    clearLCD();
    lcd.setCursor(0, 0);
    lcd.print("SETTINGS        ");
    lcd.setCursor(0, 1);
    lcd.print("Keyboard        ");
    break;
  case 320:
    clearLCD();
    lcd.setCursor(0, 0);
    lcd.print("SETTINGS        ");
    lcd.setCursor(0, 1);
    lcd.print("Trigger MIDI    ");
    break;
  case 330:
    clearLCD();
    lcd.setCursor(0, 0);
    lcd.print("SETTINGS        ");
    lcd.setCursor(0, 1);
    lcd.print("General         ");
    break;
  }
}

void updateValues()
{
  if (uiRefresh && valueChange && menu != 0)
  {
    arrowAnimation = false;
    uiRefresh = false;
    valueChange = false;
    switch (menu)
    {
    case 0: // SPLASH/MAIN
      lcd.setCursor(0, 0);
      lcd.print("Main            ");
      break;

    case 10: // OSC1
      lcd.setCursor(0, 0);
      lcd.print("Os1 Oct Lvl Det ");

      switch (osc1WaveType)
      {
      case 0:
        lcd.setCursor(0, 1);
        lcd.print("Sin ");
        break;
      case 1:
        lcd.setCursor(0, 1);
        lcd.print("Tri ");
        break;
      case 2:
        lcd.setCursor(0, 1);
        lcd.print("Saw ");
        break;
      case 3:
        lcd.setCursor(0, 1);
        lcd.print("Squ ");
        arrow(3, 1);
        break;
      case 4:
        lcd.setCursor(0, 1);
        lcd.print("Us1 ");
        arrow(3, 1);
        break;
      case 5:
        lcd.setCursor(0, 1);
        lcd.print("Us2 ");
        break;
      case 6:
        lcd.setCursor(0, 1);
        lcd.print("Us3 ");
        break;
      case 7:
        lcd.setCursor(0, 1);
        lcd.print("Nse ");
        break;
      }

      showValue(4, 1, osc1Octave); // octave

      showValue(8, 1, osc1Volume >> 2); // level

      showValue(12, 1, osc1Detune); // detune
      break;

    case 11: // OSCILLATORS - change user waveshape
      lcd.setCursor(0, 0);
      lcd.print("Us1: ");

      if (inFolder)
      {
        lcd.setCursor(4, 0);
        lcd.print(folderName);
        lcd.print("        ");
        if (justEnteredFolder)
        {
          lcd.setCursor(0, 1);
          lcd.print("Load Wave [P4] ");
        }
      }
      else
      {
        lcd.setCursor(4, 0);
        lcd.print("Root");
        lcd.print("        ");
        lcd.setCursor(0, 1);
        lcd.print("Set Folder [P4] ");
      }
      getWaveform();
      break;

    case 12: // SQUARE - adjust PWM
      lcd.setCursor(0, 0);
      lcd.print("Squ: Pulse Width");
      showValue(11, 1, (map(uiPulseWidth, ((WAVE_SAMPLES / 2) - 15) * -1, ((WAVE_SAMPLES / 2) - 15), 0, 255)));
      break;

    case 20: // OSC2
      lcd.setCursor(0, 0);
      lcd.print("Os2 Oct Lvl Det ");

      switch (osc2WaveType)
      {
      case 0:
        lcd.setCursor(0, 1);
        lcd.print("Sin ");
        break;
      case 1:
        lcd.setCursor(0, 1);
        lcd.print("Tri ");
        break;
      case 2:
        lcd.setCursor(0, 1);
        lcd.print("Saw ");
        break;
      case 3:
        lcd.setCursor(0, 1);
        lcd.print("Squ ");
        arrow(3, 1);
        break;
      case 4:
        lcd.setCursor(0, 1);
        lcd.print("Us1 ");
        break;
      case 5:
        lcd.setCursor(0, 1);
        lcd.print("Us2 ");
        arrow(3, 1);
        break;
      case 6:
        lcd.setCursor(0, 1);
        lcd.print("Us3 ");
        break;
      case 7:
        lcd.setCursor(0, 1);
        lcd.print("Nse ");
        break;
      }
      showValue(4, 1, osc2Octave); // octave

      showValue(8, 1, osc2Volume >> 2); // level

      showValue(12, 1, osc2Detune >> 14); // detune
      break;

    case 21: // OSC2 - change user waveshape
      lcd.setCursor(0, 0);
      lcd.print("Us2: ");

      if (inFolder)
      {
        lcd.setCursor(4, 0);
        lcd.print(folderName);
        lcd.print("        ");
        if (justEnteredFolder)
        {
          lcd.setCursor(0, 1);
          lcd.print("Load Wave [P4] ");
        }
      }
      else
      {
        lcd.setCursor(4, 0);
        lcd.print("Root");
        lcd.print("        ");
        lcd.setCursor(0, 1);
        lcd.print("Set Folder [P4] ");
      }
      getWaveform();
      break;

    case 22: // SQUARE - adjust PWM
      lcd.setCursor(0, 0);
      lcd.print("Squ: Pulse Width");

      showValue(11, 1, (map(uiPulseWidth, ((WAVE_SAMPLES / 2) - 15) * -1, ((WAVE_SAMPLES / 2) - 15), 0, 255)));
      break;

    case 30: // FILTER
      lcd.setCursor(0, 0);
      lcd.print("Cut Res Typ Byp");
      showValue(0, 1, filterCutoff);
      showValue(4, 1, filterResonance);
      lcd.setCursor(12, 1);
      switch (fType)
      {
      case 0:
        lcd.setCursor(8, 1);
        lcd.print("LP  ");
        break;
      case 1:
        lcd.setCursor(8, 1);
        lcd.print("BP  ");
        break;
      case 2:
        lcd.setCursor(8, 1);
        lcd.print("HP  ");
        break;
      }
      if (filterBypass)
      {
        lcd.print("On ");
      }
      else
        lcd.print("Off");
      break;

    case 40: // ENVELOPE
      lcd.setCursor(0, 0);
      lcd.print("Att Dec Sus Rel ");
      showValue(0, 1, attackTime >> 2);
      showValue(4, 1, decayTime >> 2);
      showValue(8, 1, sustainLevel >> 2);
      showValue(12, 1, releaseTime >> 2);
      break;

    case 50: // LFO
      lcd.setCursor(0, 0);
      if (!lfoSync)
        lcd.print("Shp Rt  Rng Trg");
      else
        lcd.print("Shp RtS Rng Trg");
      switch (lfoShape)
      {
      case 0:
        lcd.setCursor(0, 1);
        lcd.print("Sin ");
        break;
      case 1:
        lcd.setCursor(0, 1);
        lcd.print("Tri ");
        break;
      case 2:
        lcd.setCursor(0, 1);
        lcd.print("Saw ");
        break;
      case 3:
        lcd.setCursor(0, 1);
        lcd.print("Squ ");
        break;
      case 4:
        lcd.setCursor(0, 1);
        lcd.print("Us1 ");
        break;
      case 5:
        lcd.setCursor(0, 1);
        lcd.print("Us2 ");
        break;
      case 6:
        lcd.setCursor(0, 1);
        lcd.print("Us3 ");
        arrow(3, 1);
        break;
      }

      if (!lfoSync)
      {
        if (lfoLowRange)
        {
          showValue(4, 1, constrain(map(userLfoRate, 1, 64, 255, 1), 1, 255));
          lcd.setCursor(8, 1);
          lcd.print("Low ");
        }
        else // high range
        {
          showValue(4, 1, constrain(map(userLfoRate, 1, 1024, 255, 1), 1, 255));
          lcd.setCursor(8, 1);
          lcd.print("Hi  ");
        }
      }
      else
      {
        String syncName[5] = {"1/8 ", "1/4 ", "1/2 ", "1b  ", "2b  "};
        lcd.setCursor(4, 1);
        lcd.print(syncName[syncSelector]);
        lcd.setCursor(8, 1);
        lcd.print("n/a ");
      }

      if (retrigger)
      {
        lcd.setCursor(12, 1);
        lcd.print("On  ");
      }
      else // retrigger is false
      {
        lcd.setCursor(12, 1);
        lcd.print("Off ");
      }
      break;

    case 51: // LFO - change user waveshape
      lcd.setCursor(0, 0);
      lcd.print("Us3: ");

      if (inFolder)
      {
        lcd.setCursor(4, 0);
        lcd.print(folderName);
        lcd.print("        ");
        if (justEnteredFolder)
        {
          lcd.setCursor(0, 1);
          lcd.print("Load Wave [P4] ");
        }
      }
      else
      {
        lcd.setCursor(4, 0);
        lcd.print("Root");
        lcd.print("        ");
        lcd.setCursor(0, 1);
        lcd.print("Set Folder [P4] ");
      }
      getWaveform();
      break;

    case 60: // MODULATION
      static byte lastSource = 255;
      lcd.setCursor(0, 0);
      lcd.print("Src Dst Amt     ");
      switch (source)
      {
      case 0: // SOURCE LFO
        if (lastSource != source)
        {
          destination = 0;
          lcd.setCursor(0, 1);
          lcd.print("                ");
          lastSource = source;
        }
        lcd.setCursor(0, 1);
        lcd.print("LFO ");

        switch (destination)
        {
        case 0:
          lcd.setCursor(4, 1);
          lcd.print("Os1 ");
          showValue(8, 1, lfoOsc1DetuneFactor >> 2);
          break;
        case 1:
          lcd.setCursor(4, 1);
          lcd.print("Os2 ");
          showValue(8, 1, lfoOsc2DetuneFactor >> 2);
          break;
        case 2:
          lcd.setCursor(4, 1);
          lcd.print("Oc1 ");
          showValue(8, 1, osc1OctaveMod);
          break;
        case 3:
          lcd.setCursor(4, 1);
          lcd.print("Oc2 ");
          showValue(8, 1, osc2OctaveMod);
          break;
        case 4:
          lcd.setCursor(4, 1);
          lcd.print("Cut ");
          showValue(8, 1, lfoAmount >> 2);
          break;
        case 5:
          lcd.setCursor(4, 1);
          lcd.print("Amp ");
          showValue(8, 1, lfoAmpFactor >> 2);
          break;
        case 6:
          lcd.setCursor(4, 1);
          lcd.print("PWi ");
          showValue(8, 1, lfoPwFactor >> 2);
          break;
        }
        break;

      case 1: // SOURCE ENVELOPE
      {
        if (lastSource != source)
        {
          destination = 7;
          lcd.setCursor(0, 1);
          lcd.print("                ");
          lastSource = source;
        }
        lcd.setCursor(0, 1);
        lcd.print("Env ");
        switch (destination)
        {
        case 7:
          lcd.setCursor(4, 1);
          lcd.print("Os1 ");
          showValue(8, 1, envOsc1PitchFactor >> 4);
          break;
        case 8:
          lcd.setCursor(4, 1);
          lcd.print("Os2 ");
          showValue(8, 1, envOsc2PitchFactor >> 4);
          break;
        case 9:
          lcd.setCursor(4, 1);
          lcd.print("Cut ");
          showValue(8, 1, envFilterCutoffFactor >> 4);
          break;
        case 10:
          lcd.setCursor(4, 1);
          lcd.print("LRt ");
          showValue(8, 1, envLfoRate >> 2);
          break;
        }
      }
      break;

      case 2: // SOURCE VELOCITY
      {
        if (lastSource != source)
        {
          destination = 11;
          lcd.setCursor(0, 1);
          lcd.print("                ");
          lastSource = source;
        }
        lcd.setCursor(0, 1);
        lcd.print("Vel ");
        switch (destination)
        {
        case 11:
          lcd.setCursor(4, 1);
          lcd.print("Os1 ");
          showValue(8, 1, velOsc1DetuneFactor >> 4);
          break;
        case 12:
          lcd.setCursor(4, 1);
          lcd.print("Os2 ");
          showValue(8, 1, velOsc2DetuneFactor >> 4);
          break;
        case 13:
          lcd.setCursor(4, 1);
          lcd.print("Cut ");
          showValue(8, 1, velCutoffFactor >> 2);
          break;
        case 14:
          lcd.setCursor(4, 1);
          lcd.print("Amp ");
          showValue(8, 1, velAmpFactor >> 2);
          break;
        case 15:
          lcd.setCursor(4, 1);
          lcd.print("PWi ");
          showValue(8, 1, velPwFactor);
          break;
        case 16:
          lcd.setCursor(4, 1);
          lcd.print("LRt ");
          showValue(8, 1, velLfoRateFactor >> 2);
          break;
        }
      }
      break;
      }
      break;

    case 65: // SHAPER & GAIN
      lcd.setCursor(0, 0);
      lcd.print("Shp Amt Bit Gain");
      lcd.setCursor(0, 1);
      lcd.print("                ");
      lcd.setCursor(0, 1);
      if (shaperType == 0)
        lcd.print("Off ");
      else
        lcd.print(shaperType);
      lcd.setCursor(4, 1);
      switch (shaperType)
      {
      case 0:
        lcd.print("n/a ");
        break;
      case 1:
        showValue(4, 1, (int)(waveShapeAmount * 255));
        break;
      case 2:
        showValue(4, 1, waveShapeAmount2);
        break;
      }
      //lcd.setCursor(8, 1);
      showValue(8, 1, bitMuncher);
      //lcd.setCursor(12, 1);
      showValue(12, 1, (int)((gainAmount - 1) * 255));
      break;

    case 68: // MONO & PORTA
      lcd.setCursor(0, 0);
      lcd.print("Mon Uni Spr Por ");
      lcd.setCursor(0, 1);
      lcd.print("                ");
      lcd.setCursor(0, 1);
      if (!monoMode)
        lcd.print("Off n/a n/a ");
      else
      {
        lcd.setCursor(0, 1);
        switch (monoMode)
        {
        case 1:
          lcd.print(" Hi ");
          break;
        case 2:
          lcd.print("Low ");
          break;
        case 3:
          lcd.print("Lst ");
          break;
        }
        lcd.setCursor(4, 1);
        switch (unison)
        {
        case 0:
          lcd.print("Off ");
          break;
        case 1:
          lcd.print("2Vc ");
          break;
        case 2:
          lcd.print("3Vc ");
          break;
        case 3:
          lcd.print("4Vc ");
          break;
        }
        lcd.setCursor(8, 1);
        if (unison)
          lcd.print(map(uniSpread, 10000, 60000, 0, 255));
        else
          lcd.print("n/a ");
      }
      lcd.setCursor(12, 1);
      if (portamento > 0)
        lcd.print(portamento);
      else
        lcd.print("Off");
      break;

    case 70: // SYNTH LOAD
      lcd.setCursor(0, 0);
      lcd.print("Syn: ");

      if (inFolder)
      {
        lcd.setCursor(4, 0);
        lcd.print(folderName);
        lcd.print("        ");
        if (justEnteredFolder)
        {
          lcd.setCursor(0, 1);
          lcd.print("Load Patch [P4] ");
        }
      }
      else
      {
        lcd.setCursor(4, 0);
        lcd.print("Root");
        lcd.print("        ");
        lcd.setCursor(0, 1);
        lcd.print("Set Folder [P4] ");
      }
      getSynthPatch();
      break;

    case 80: // SYNTH SAVE
      lcd.setCursor(0, 0);
      lcd.print("Syn: ");

      if (inFolder)
      {
        lcd.setCursor(4, 0);
        lcd.print(folderName);
        lcd.print("        ");
      }
      else
      {
        lcd.setCursor(4, 0);
        lcd.print("Root");
        lcd.print("        ");
        lcd.setCursor(0, 1);
        lcd.print("Set Folder [P4] ");
      }
      saveSynthPatch();
      break;

    case 100: // ARP PAGE 1
      lcd.setCursor(0, 0);
      lcd.print("Arp Div Dur BPM ");
      lcd.setCursor(0, 1);
      switch (arp)
      {
      case 0:
        lcd.print("Off ");
        break;
      case 1:
        lcd.print("On  ");
        break;
      }
      lcd.setCursor(4, 1);
      switch (arpDivSelection)
      {
      case 0:
        lcd.print(" 4  ");
        break;
      case 1:
        lcd.print(" 4t ");
        break;
      case 2:
        lcd.print(" 8  ");
        break;
      case 3:
        lcd.print(" 8t ");
        break;
      case 4:
        lcd.print("16  ");
        break;
      case 5:
        lcd.print("16t ");
        break;
      case 6:
        lcd.print("32  ");
        break;
      }
      showValue(8, 1, arpNoteDur);
      showValue(12, 1, bpm);
      break;

    case 110: // ARP PAGE 2
      lcd.setCursor(0, 0);
      lcd.print("Dir Typ Oct     ");
      lcd.setCursor(0, 1);
      switch (arpForward)
      {
      case 0:
        lcd.print("Rev ");
        break;
      case 1:
        lcd.print("Fwd  ");
        break;
      }
      lcd.setCursor(4, 1);
      switch (arpIncrement)
      {
      case 0:
        lcd.print("Rnd ");
        break;
      case 1:
        lcd.print("1St ");
        break;
      case 2:
        lcd.print("2St ");
        break;
      case 3:
        lcd.print("3St ");
        break;
      case 4:
        lcd.print("2B1 ");
        break;
      }
      showValue(8, 1, arpOctaves);
      break;

    case 200: // SEQ TRIGGER
      lcd.setCursor(0, 0);
      lcd.print("1 2 3 4 5 6 7 8 ");
      lcd.setCursor(0, 1);
      if (seqRunning)
        lcd.print("STOP ");
      else
        lcd.print("PLAY ");
      if (currentSeq != selectedSeq)
      {
        lcd.setCursor(selectedSeq * 2 + 1, 0);
        lcd.print((char)127);
      }
      arrow(currentSeq * 2 + 1, 0);
      showValue(15, 1, selectedSeq);
      lcd.setCursor(5, 1);
      switch (seqPlayMode)
      {
      case 0:
        lcd.print("Fwd ");
        break;
      case 1:
        lcd.print("Rev ");
        break;
      case 2:
        lcd.print("Pen ");
        break;
      case 3:
        lcd.print("RIn ");
        break;
      case 4:
        lcd.print("Dnk ");
        break;
      case 5:
        lcd.print("Rnd ");
        break;
      }
      lcd.setCursor(9, 1);
      lcd.print("Tr:  ");
      showValue(13, 1, seq[currentSeq].transpose);
      break;

    case 210: // SEQ EDIT
      showSequence();
      doSeqBlink = true;
      lcd.setCursor(0, 1);
      if (seqRunning)
        lcd.print("STOP ");
      else
        lcd.print("PLAY ");
      lcd.setCursor(7, 1);
      lcd.print("Step: ");
      showValue(13, 1, seqEditStep + 1);
      if (seqEditStep < 9)
        arrow(14, 1);
      else
        arrow(15, 1);
      break;

    case 211: // SEQ EDIT NOTES
      lcd.setCursor(0, 0);
      lcd.print("V1  V2  V3  V4  ");
      for (byte i = 0; i < 4; i++)
      {
        if (seq[currentSeq].voice[i][seqEditStep] < 255)
        {
          lcd.setCursor(i * 4, 1);
          lcd.print(noteName[(seq[currentSeq].voice[i][seqEditStep] + 60) % 12]);
          lcd.print((seq[currentSeq].voice[i][seqEditStep] + 60) / 12 - 1);
          lcd.print(" ");
        }
        else
        {
          lcd.setCursor(i * 4, 1);
          lcd.print("    ");
        }
      }
      break;

    case 220: // SEQ SETTINGS
      lcd.setCursor(0, 0);
      lcd.print("Len Dur Swi BPM ");
      lcd.setCursor(0, 1);
      lcd.print("                ");
      showValue(0, 1, seq[currentSeq].patternLength);
      showValue(4, 1, seq[currentSeq].noteDur >> 2);
      showValue(8, 1, seq[currentSeq].swing >> 2);
      showValue(12, 1, seq[currentSeq].bpm);
      break;

    case 230: // SEQ BANK SETTINGS
      lcd.setCursor(0, 0);
      lcd.print("Mode            ");
      lcd.setCursor(0, 1);
      lcd.print("                ");
      lcd.setCursor(0, 1);
      switch (bankMode)
      {
      case 0:
        lcd.print("Lp1  ");
        break;
      case 1:
        lcd.print("Lp2  ");
        break;
      case 2:
        lcd.print("Lp4  ");
        break;
      case 3:
        lcd.print("Lp8  ");
        break;
      case 4:
        lcd.print("Rnd  ");
        break;
      }
      lcd.setCursor(5, 0);
      lcd.print("Clock ");
      if (receivingClock)
        showValue(5, 1, bpm);
      else
      {
        lcd.setCursor(5, 1);
        lcd.print("No    ");
      }
      //showValue(4, 1, seq[currentSeq].noteDur >> 2);
      //showValue(8, 1, seq[currentSeq].swing >> 2);
      //showValue(12, 1, seq[currentSeq].bpm);
      break;

    case 250: // SEQUENCER BANK LOAD
      lcd.setCursor(0, 0);
      lcd.print("Seq: ");

      if (inFolder)
      {
        lcd.setCursor(4, 0);
        lcd.print(folderName);
        lcd.print("        ");
        if (justEnteredFolder)
        {
          lcd.setCursor(0, 1);
          lcd.print("Set Bank  [P4] ");
        }
      }
      else
      {
        lcd.setCursor(4, 0);
        lcd.print("Root");
        lcd.print("        ");
        lcd.setCursor(0, 1);
        lcd.print("Set Folder [P4] ");
      }
      getSeqBank();
      break;

    case 260: // SEQUENCER BANK SAVE
      lcd.setCursor(0, 0);
      lcd.print("Seq: ");

      if (inFolder)
      {
        lcd.setCursor(4, 0);
        lcd.print(folderName);
        lcd.print("        ");
      }
      else
      {
        lcd.setCursor(4, 0);
        lcd.print("Root");
        lcd.print("        ");
        lcd.setCursor(0, 1);
        lcd.print("Set Folder [P4] ");
      }
      saveSeqBank();
      break;

    case 300: // SETTINGS MIDI
      lcd.setCursor(0, 0);
      lcd.print("Out Chn Thr Sync");
      lcd.setCursor(0, 1);
      lcd.print("                ");
      if (midiOut)
      {
        lcd.setCursor(0, 1);
        lcd.print("Yes ");
      }
      else
      {
        lcd.setCursor(0, 1);
        lcd.print("No  ");
      }
      showValue(4, 1, midiChannel);
      lcd.setCursor(8, 1);
      switch (midiThruType)
      {
      case 0:
        lcd.print("Off ");
        break;
      case 1:
        lcd.print("All ");
        break;
      case 2:
        lcd.print("Clk ");
        break;
      }
      lcd.setCursor(12, 1);
      switch (midiSync)
      {
      case 0:
        lcd.print("None");
        break;
      case 1:
        lcd.print("Mst");
        break;
      case 2:
        lcd.print("Slv");
        break;
      }
      break;

    case 310: // SETTINGS KEYBOARD
      lcd.setCursor(0, 0);
      lcd.print("Out Vel         ");
      lcd.setCursor(0, 1);
      lcd.print("                ");
      if (keysOut)
      {
        lcd.setCursor(0, 1);
        lcd.print("Yes ");
      }
      else
      {
        lcd.setCursor(0, 1);
        lcd.print("No  ");
      }
      showValue(4, 1, keyVelocity);
      break;

    case 320: // SETTINGS TRIGGER MIDI
      lcd.setCursor(0, 0);
      lcd.print("Trg Chn No. Note");
      lcd.setCursor(0, 1);
      lcd.print("                ");
      if (midiTriggerOut)
      {
        lcd.setCursor(0, 1);
        lcd.print("Yes ");
      }
      else
      {
        lcd.setCursor(0, 1);
        lcd.print("No  ");
      }
      showValue(4, 1, midiTriggerChannel);
      showValue(8, 1, editTrigger + 1);
      showValue(12, 1, midiTrigger[editTrigger]);
      break;

    case 330: // SETTINGS GENERAL
      lcd.setCursor(0, 0);
      lcd.print("Vol             ");
      lcd.setCursor(0, 1);
      lcd.print("                ");
      showValue(0, 1, volume >> 2);
      break;
    }
  }
}
void arrow(byte x, byte y)
{
  arrowAnimation = true;
  arrowX = x;
  arrowY = y;
}

void arrowAnim()
{
  static byte lastArrowFrame = 255;                   // can't initialize to 0
  byte arrowMap[10] = {0, 0, 0, 0, 0, 1, 2, 3, 2, 1}; // the sequence of frames for the arrow animation
  if (arrowAnimation && arrowFrame != lastArrowFrame)
  {
    lastArrowFrame = arrowFrame;
    lcd.setCursor(arrowX, arrowY);
    switch (arrowMap[arrowFrame])
    {
    case 0:
      lcd.write(byte(0));
      break;
    case 1:
      lcd.write((byte)1);
      break;
    case 2:
      lcd.write(2);
      break;
    case 3:
      lcd.write(3);
      break;
    }
  }
}

void showTB2(byte x) // horizontal position
{
  lcd.setCursor(x, 0);
  lcd.write(2);
  lcd.write(4);
  lcd.write(6);
  lcd.setCursor(x, 1);
  lcd.write(3);
  lcd.write(5);
  lcd.write(7);
}

void showSequence() // displays the sequence currently being edited
{
  for (byte i = 0; i < 16; i++)
    showStep(i);
}

void seqBlinker()
{
  if (menu == 210 && !seqRunning && doSeqBlink)
  {
    static boolean lastSeqBlink = 0;
    if (seqBlink != lastSeqBlink)
    {
      lastSeqBlink = seqBlink;
      lcd.setCursor(seqEditStep, 0);
      if (seqBlink)
        lcd.print(" ");
      else
        showStep(seqEditStep);
    }
  }
}

void showStep(byte Step)
{
  lcd.setCursor(Step, 0);
  if (seq[currentSeq].mute[Step])
    lcd.write(6);
  else if (seq[currentSeq].tie[Step])
    lcd.write(7);
  //lcd.print((char)126); // forward arrow
  else if (seq[currentSeq].voice[0][Step] == 255)
    lcd.write(5);
  else
    lcd.write(4);
}

void clearLCD()
{
  lcd.setCursor(0, 0);
  lcd.print("                ");
  lcd.setCursor(0, 1);
  lcd.print("                ");
}

// VELOCITY.ino

// *** VELOCITY ***
// we calculate velocity modulation variables as and when we receive velocity data
// ie. we don't have to calculate it in the LFO interrupt, we just pass the value there

void setVeloModulation(byte velocity)
{
  if (velOsc1DetuneFactor != 0)
    velOsc1Detune = (velocity * velOsc1DetuneFactor) << 6;
  else
    velOsc1Detune = 0;

  if (velOsc2DetuneFactor != 0)
    velOsc2Detune = (velocity * velOsc2DetuneFactor) << 6;
  else
    velOsc2Detune = 0;

  if (velCutoffFactor != 0)
    velCutoff = map(velocity, 0, 127, 0, velCutoffFactor >> 2);
  else
    velCutoff = 0;

  if (velAmpFactor != 0)
    tempVelAmp = map(velocity, 0, 127, velAmpFactor, 0);
  else
    tempVelAmp = 0; // this gets added to lfoAmp

  if (velPwFactor != 0)
    velPw = map(velocity, 0, 127, 0, velPwFactor);
  else
    velPw = 0;

  if (velLfoRateFactor != 0)
    velLfoRate = constrain(lfoRate - map(velLfoRateFactor, 0, 1023, 0, velocity), 0, lfoRate);
  else
    velLfoRate = lfoRate;
}

// WAVESHAPER.ino

// *** WAVESHAPER***
// the floating point math executes too slowly to use in the audio interrupt directly
// so we pre-calculate a waveshaper lookup table outside of the audio loop and just plug in the current sample value in the audio interrupt

void createWaveShaper()
{
  if (shaperType == 0) // off - no change
  {
    for (uint16_t sampleInput = 0; sampleInput < 4096; sampleInput++)
      waveShaper[sampleInput] = sampleInput;
  }

  else if (shaperType == 1) // http://www.musicdsp.org/showArchiveComment.php?ArchiveID=46
  {
    float k = 2 * waveShapeAmount / (1 - waveShapeAmount);
    for (uint16_t sampleInput = 0; sampleInput < 4096; sampleInput++)
    {
      float x = (float)(sampleInput - 2048) / 2048;
      float y = (1 + k) * x / (1 + k * abs(x));
      float tmpOutput = (y * 2048) + 2048;
      uint16_t sampleOutput = (uint16_t)tmpOutput;
      waveShaper[sampleInput] = sampleOutput;
    }
  }

  else if (shaperType == 2) // http://www.musicdsp.org/showArchiveComment.php?ArchiveID=41
  {
    float a = (float)waveShapeAmount2;
    for (uint16_t sampleInput = 0; sampleInput < 4096; sampleInput++)
    {
      float x = (float)(sampleInput - 2048) / 2048;
      float y = x * (abs(x) + a) / (pow(x, 2) + (a - 1) * abs(x) + 1);
      float tmpOutput = (y * 2048) + 2048;
      uint16_t sampleOutput = (uint16_t)tmpOutput;
      waveShaper[sampleInput] = sampleOutput;
    }
  }
}
