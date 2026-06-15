/**
 * InputManager.h — header-only library
 * Plant Light Logger project
 *
 * Debounces 4 tactile buttons (INPUT_PULLUP, active LOW) and normalizes a
 * 2-axis analog joystick into discrete directional ticks for UI navigation.
 *
 * Hardware connections (ATmega2560):
 *   Button A        → Pin 22     Button B     → Pin 23
 *   Button VIEW     → Pin 24     Button SET   → Pin 25
 *   Joystick X-axis → A0         Joystick Y-axis → A1
 *
 * Usage:
 *   1. #include "../../lib/InputManager.h"   (adjust relative path as needed)
 *   2. Declare:  InputManager inputMgr;
 *   3. setup():  inputMgr.init();
 *   4. loop():   inputMgr.update();   // must be first call every loop
 *                inputMgr.isButtonPressed(ButtonId::A);      // fire-once tap event
 *                inputMgr.isButtonHeld(ButtonId::VIEW);      // continuous hold state
 *                inputMgr.getJoystickDirection();            // rate-limited tick
 *                inputMgr.getRawJoystickDir();               // instantaneous direction
 *
 * isButtonPressed()     — returns true ONCE per physical press (cleared on read).
 *                         Holding the button does not re-fire.
 *
 * isButtonHeld()        — returns the current debounced hold state.  Useful for
 *                         display indicators that should stay lit while held.
 *
 * getJoystickDirection()— fires once immediately when the stick leaves centre,
 *                         then auto-repeats every JOY_REPEAT_MS while held.
 *                         Returns NONE between ticks and when centred.
 *
 * getRawJoystickDir()   — instantaneous direction without rate-limiting.  Useful
 *                         for display indicators that should stay lit while deflected.
 * 
 * MINIMAL SKETCH
#include <Arduino.h>
#include "../../lib/InputManager.h"

InputManager inputMgr;

void setup() {
    Serial.begin(115200);
    inputMgr.init();
}

void loop() {
    inputMgr.update();

    if (inputMgr.isButtonPressed(ButtonId::A))
        Serial.println(F("A tapped"));

    JoystickDir dir = inputMgr.getJoystickDirection();
    if (dir == JoystickDir::LEFT)
        Serial.println(F("scroll left"));
}

 */

#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------------------
// Public enumerations
// ---------------------------------------------------------------------------

enum class ButtonId    : uint8_t { A = 0, B = 1, VIEW = 2, SETTINGS = 3 };
enum class JoystickDir : uint8_t { NONE = 0, UP, DOWN, LEFT, RIGHT };

// ---------------------------------------------------------------------------
// Class declaration
// ---------------------------------------------------------------------------

class InputManager {
public:
    void init();
    void update();

    bool        isButtonPressed(ButtonId btn);  // fire-once press event, clears on read
    bool        isButtonHeld(ButtonId btn);     // current debounced hold state

    JoystickDir getJoystickDirection();         // rate-limited tick, clears on read
    JoystickDir getRawJoystickDir() const;      // instantaneous direction, no side-effects

private:
    // --- Configuration constants ---
    static const uint8_t        NUM_BTNS        = 4;
    static const int            PIN_JOY_X       = A0;
    static const int            PIN_JOY_Y       = A1;
    static const int            THRESHOLD_LOW   = 200;   // joystick deflection deadband
    static const int            THRESHOLD_HIGH  = 823;
    static const unsigned long  DEBOUNCE_MS     = 35;    // button debounce window
    static const unsigned long  JOY_REPEAT_MS   = 250;   // auto-repeat interval

    // Returns the digital pin number for button index i.
    // Defined as a static method with a local array to avoid static-member ODR issues
    // in header-only libraries on older avr-gcc toolchains.
    static uint8_t _btnPin(uint8_t i) {
        const uint8_t pins[NUM_BTNS] = {22, 23, 24, 25};
        return pins[i];
    }

    // --- Button state ---
    bool          _lastRaw[NUM_BTNS];       // raw digitalRead result last cycle
    bool          _stable[NUM_BTNS];        // current debounced hold state
    bool          _pressedEvent[NUM_BTNS];  // rising-edge flag (cleared by isButtonPressed)
    unsigned long _debounceStart[NUM_BTNS]; // timestamp when raw state last changed

    // --- Joystick state ---
    JoystickDir   _rawJoyDir;      // current instantaneous direction
    JoystickDir   _lastJoyDir;     // direction used last cycle (for edge detection)
    JoystickDir   _pendingJoyTick; // pending rate-limited tick (cleared by getJoystickDirection)
    unsigned long _lastJoyTick;    // timestamp of the most recent tick emission
};

// ---------------------------------------------------------------------------
// Implementation (all methods are inline to satisfy ODR in header-only use)
// ---------------------------------------------------------------------------

inline void InputManager::init() {
    for (uint8_t i = 0; i < NUM_BTNS; i++) {
        pinMode(_btnPin(i), INPUT_PULLUP);
        _lastRaw[i]       = false;
        _stable[i]        = false;
        _pressedEvent[i]  = false;
        _debounceStart[i] = 0;
    }
    _rawJoyDir      = JoystickDir::NONE;
    _lastJoyDir     = JoystickDir::NONE;
    _pendingJoyTick = JoystickDir::NONE;
    _lastJoyTick    = 0;
}

inline void InputManager::update() {
    unsigned long now = millis();

    // --- Debounce buttons ---
    for (uint8_t i = 0; i < NUM_BTNS; i++) {
        bool raw = (digitalRead(_btnPin(i)) == LOW);  // active LOW (INPUT_PULLUP)

        if (raw != _lastRaw[i]) {
            // Raw state changed — start (or restart) the debounce window.
            _lastRaw[i]       = raw;
            _debounceStart[i] = now;
        }

        if ((now - _debounceStart[i]) >= DEBOUNCE_MS) {
            bool wasStable = _stable[i];
            _stable[i] = raw;

            // Rising edge: transition to pressed → set the fire-once event flag.
            if (_stable[i] && !wasStable) {
                _pressedEvent[i] = true;
            }
        }
    }

    // --- Normalise joystick axes into a direction ---
    int xVal = analogRead(PIN_JOY_X);
    int yVal = analogRead(PIN_JOY_Y);

    // Y-axis is checked first; diagonal positions default to vertical movement.
    JoystickDir dir = JoystickDir::NONE;
    if      (yVal > THRESHOLD_HIGH) dir = JoystickDir::UP;
    else if (yVal < THRESHOLD_LOW)  dir = JoystickDir::DOWN;
    else if (xVal < THRESHOLD_LOW)  dir = JoystickDir::LEFT;
    else if (xVal > THRESHOLD_HIGH) dir = JoystickDir::RIGHT;

    _rawJoyDir = dir;

    if (dir != JoystickDir::NONE) {
        if (dir != _lastJoyDir) {
            // New direction — emit a tick immediately and reset the repeat timer.
            _lastJoyDir     = dir;
            _lastJoyTick    = now;
            _pendingJoyTick = dir;
        } else if ((now - _lastJoyTick) >= JOY_REPEAT_MS) {
            // Same direction held long enough — emit an auto-repeat tick.
            _lastJoyTick    = now;
            _pendingJoyTick = dir;
        }
    } else {
        // Joystick returned to centre — reset so the next deflection fires immediately.
        _lastJoyDir = JoystickDir::NONE;
    }
}

inline bool InputManager::isButtonPressed(ButtonId btn) {
    uint8_t i = static_cast<uint8_t>(btn);
    if (_pressedEvent[i]) {
        _pressedEvent[i] = false;
        return true;
    }
    return false;
}

inline bool InputManager::isButtonHeld(ButtonId btn) {
    return _stable[static_cast<uint8_t>(btn)];
}

inline JoystickDir InputManager::getJoystickDirection() {
    JoystickDir d   = _pendingJoyTick;
    _pendingJoyTick = JoystickDir::NONE;
    return d;
}

inline JoystickDir InputManager::getRawJoystickDir() const {
    return _rawJoyDir;
}
