# Buzzer

Cleanflight supports a buzzer which is used for the following purposes, and more:

Low Battery alarm (when battery monitoring enabled)
Notification of calibration complete status.
AUX operated beeping - useful for locating your aircraft after a crash.
Failsafe status.

Buzzer is enabled by default on platforms that have buzzer connections.

## Types of buzzer supported

The buzzers are enabled/disabled by simply enabling or disabling a GPIO output pin on the board.
This means the buzzer must be able to generate it's own tone simply by having power applied to it.

Buzzers that need an analogue or PWM signal do not work and will make clicking noises or no sound at all.

Example of a known-working buzzer.

http://www.rapidonline.com/Audio-Visual/Hcm1205x-Miniature-Buzzer-5v-35-0055

