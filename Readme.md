## To execute:
    Upload the sketch and run the mic bridge python script. 
    Serial port should not be used by any other application while running py script.

## Steps to Get Started


## Required Softwares: Arduino IDE
https://downloads.arduino.cc/arduino-ide/arduino-ide_2.3.7_Windows_64bit.exe


## Board Configurations Required:
1) esp32 by  Espressif Systems

![alt text](image.png)

## To fix connection timeout:
    Close Arduino IDE completely. Then proceed with below steps:

    Win + R - opens ms run
    Type appdata >> press ENTER
    Press backspace - this will lead to your user root folder (C:\Users\username)
    Open the .arduinoIDE folder
    Open the arduino-cli.yaml file and add the parameter at the end:
    
    `
    network:
            connection_timeout: 1800s 
    `
    Save the cofig and try to download the board configs and teh libraries.

    If it still fails to downlaod and throws the error connection timed out then try switch to an better stable network.



## Libraries Required:


1) ArduinoHttpClient by Arduino

![alt text](image-1.png)

2) Arduino_DebugUtils by Arduino

![alt text](image-2.png)

3) Arduino_ESP32_OTA by Arduino

![alt text](image-3.png)

4) TensorFlowLite_Esp32 by TensorFlow Authors

![alt text](image-4.png)



## Monitor terminal logs
Open Arduino IDE. Then proceed the steps below:

 Tools>> Serial Monitor>> Baud Rate>> 115200



## Get an example of basic TinyMl scripts init and get started:

Open Arduino IDE. Then proceed the steps below:

  File>> Examples >>TensorFlowLite_ESP32 >>hello_world


### Docs for ESP32S2-DevkitM-1
https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s2/esp32-s2-devkitm-1/user_guide.html
