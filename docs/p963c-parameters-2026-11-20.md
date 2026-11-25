# Piazzetta P963 C parameters, read 2026-11-20 00:2x over the radio

Read by the ESP board (press_keys, door flag) via MENU > PARAMETER MENU > FACTORY SETTINGS > KEY ACCESS E9.
Stove running at the time. Values only viewed (SET steps), nothing changed. Compare: Pascal's P96X sheet (P96X-parameters-Pascal-Maertens-2015.pdf).

| P | Panel text | Value | P96X factory | P96X locked range |
|---|---|---|---|---|
| 01 | MINUTES TIME-OUT | 30 min | 30 | 20-30 |
| 02 | MINUTES START | 5 min | 5 | 2-8 |
| 03 | CADENCE CLEANING | 20 min | 60 (Pascal: 45) | 30-90 |
| 04 | AUGER LIGHTS | 3.2 s | 3.0 | 2.7-4 |
| 05 | AUGER START | 3.8 s | 3.8 | 3-5 |
| 06 | AUGER POWER 1 | 2.7 s | 3.2 | 2.7-4 |
| 07 | AUGER POWER 2 | 3.5 s | 3.9 | 3.5-5 |
| 08 | AUGER POWER 3 | 4.5 s | 5.0 | 4.5-6 |
| 09 | AUGER POWER 4 | 5.5 s | 5.9 | 5.5-7 |
| 10 | AUGER POWER 5 | 6.0 s | 7.1 | 6-8.2 |
| 11 | DELAY ALARMS | 90 s | 51 (Pascal: 90) | 20-90 |
| 12 | DURATION CLEANING | 30 s | 20 (Pascal: 24) | 10-30 |
| 13 | THRESHOLD MINIMUM | 104 F (40 C) | 43 C | 40-50 C |
| 14 | THRESHOLD MAXIMUM | 345 F (174 C) | 185 C | 160-190 C |
| 15 | THRESHOLD BLOWER | 185 F (85 C) | 85 C | 70-90 C |
| 16 | SPEED SMOKE LIG | 2020 rpm | 2500? (sheet "25") | 1700-2300 |
| 17 | SPEED SMOKE STA | 2050 rpm | 2200? ("22") | 1700-2300 |
| 18 | SPEED SMOKE P1 | 2100 rpm | 1300? ("13") | 1500-2100 |
| 19 | SPEED SMOKE P2 | 2300 rpm | 1400? ("14") | 1700-2300 |
| 20 | SPEED SMOKE P3 | 2400 rpm | 1500? ("15") | 1900-2400 |
| 21 | SPEED SMOKE P4 | 2600 rpm | 1700? ("17") | 2050-2600 |
| 22 | SPEED SMOKE P5 | 2800 rpm | 2000? ("20") | 2350-2800 |
| 23-27 | SPEED BLOWER 1-5 | 55 each | 55 | |
| 28 | THRESHOLD HOT TEMP | 374 F (190 C) | 200 C | do not touch |

Units on screen: "\" = minutes, "]" = seconds. Decimal point = bit 7 on a digit.
Observation: feed P6-P10 all sit at the locked MINIMUM; exhaust fan P18-P22 all at the locked MAXIMUM (lean: least fuel, most air).
The sheet's P96X fan column ("25, 22, 13...") does not match these rpm values; its unit is unclear.
