import paho.mqtt.client as mqtt
import math
import json
from time import sleep

def on_connect(client, userdata, flags, rc):
    print("Connected with result code "+str(rc))

client = mqtt.Client()
client.on_connect = on_connect

client = mqtt.Client("PlotJuggler-test")

client.connect("127.0.0.1", 1883, 60)

time = 0.0

while True:
    sleep(0.20)
    time += 0.20
    data = {
        "timestamp": time,
        "test_data": {
            "cos": math.cos(time),
            "sin": math.sin(time)
        }
    }

    ret = client.publish("plotjuggler/stuff", json.dumps(data), qos=0 )
    print( ret.is_published() )
