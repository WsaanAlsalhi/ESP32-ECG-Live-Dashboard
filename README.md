# ESP32-ECG-Live-Dashboard
This project is a DIY ECG system using ESP32, capable of real-time ECG waveform visualization and heart rate (BPM) monitoring through a web interface. It supports both automatic peak detection and manual/tap modes for flexible heartbeat measurement.
1. Live ECG plotting via WebSockets
2. Automatic BPM calculation from detected peaks
3. Tap Mode & Manual Mode for user input
4. Lightweight, runs entirely on ESP32

## Features
| Feature             | Description                                         |
| ------------------- | --------------------------------------------------- |
| Real-time ECG       | Plots live ECG waveform on web dashboard            |
| BPM Calculation     | Computes heart rate from peak intervals             |
| Tap Mode            | Calculate BPM by tapping along with heartbeat       |
| Manual Mode         | Calculate BPM from manually counted beats & seconds |
| Adjustable Sampling | Default ~500Hz, configurable in code                |
| DIY Friendly        | Works with AD8232 or similar ECG sensors            |

## Hardware Required
 1. ESP32 (any ADC-capable board, e.g., GPIO34 used)
 2. ECG sensor (e.g., AD8232)
 3. Optional: Mini oscilloscope for verification

## Software Requirements
 Arduino IDE

## Libraries:
1. WiFi.h
2. WebServer.h
3. WebSocketsServer.h
4. Ticker.h

## How It Works
1. ESP32 reads ECG signal from ADC at ~500 Hz.
2. Samples are stored in a circular buffer.
3. Peaks are detected using a lightweight algorithm.
4. BPM is computed from peak intervals.
5. Sample batches and BPM are streamed via WebSockets.
6. Web interface shows:
    Real-time waveform
    Current BPM
    Tap/Manual modes for custom measurements

## Web Interface
1. Responsive HTML/CSS/JS dashboard
2. Real-time plotting with history buffer
3. Clear button to reset waveform
4. Tap mode: calculate BPM interactively
5. Manual mode: calculate BPM from user-entered beats & duration

⚠ Note: Experimental system. Verify signals with an oscilloscope before connecting to humans.

## Configuration
1. WiFi AP: Change AP_SSID & AP_PASS in code
2. ADC pin: Default GPIO34 (ADC1_CH6)
3. Sampling period: SAMPLE_PERIOD_US (~500 Hz)
4. Peak detection parameters: MIN_PEAK_DISTANCE_MS, PEAK_THRESHOLD_REL

## Usage
1. Upload the code to ESP32.
2. Connect ECG sensor to ADC pin.
3. Connect to ESP32 WiFi AP (Pulse-ESP32).
4. Open browser at http://192.168.4.1/ .
5. Monitor live ECG and BPM.
