import paho.mqtt.client as mqtt
from paho.mqtt.enums import CallbackAPIVersion
import math
import json
from time import sleep

# The callback for when the client receives a CONNACK response from the server.
def on_connect(client, userdata, flags, reason_code, properties):
    print("Connected with result code "+str(reason_code))

client = mqtt.Client(callback_api_version=CallbackAPIVersion.VERSION2, client_id="PlotJuggler-test")
client.on_connect = on_connect

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
