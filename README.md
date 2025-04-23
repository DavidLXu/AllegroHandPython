Allegro Hand Python API
==========================
Note: This project works only for PEAK System CAN interface (chardev) for USB: PCAN-USB

This code is a wrapper on https://github.com/simlabrobotics/allegro_hand_linux_v4.

Build the above C++ code first. After building, we have `./build/grasp/grasp` as a binary executable which is used in the python interface in this repo.

Install Python libs

```
pip install numpy pygame
```

Run Python API
```
python allegro_hand_client.py
```
