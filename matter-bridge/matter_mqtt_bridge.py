#!/usr/bin/env python3
"""
Pont MQTT <-> Matter : pilote une lampe Matter avec chip-tool.

  home/light/set           commandes : ON, OFF, TOGGLE, ou un niveau 0-254
  home/light/state         etat JSON {"on": true, "level": 128} (retenu)
  home/light/availability  online / offline (retenu, testament MQTT)
"""
import json
import os
import queue
import re
import subprocess
import threading

import paho.mqtt.client as mqtt

CHIP_TOOL = os.path.expanduser("~/connectedhomeip/out/chip-tool/chip-tool")
NODE_ID = "1"          # Node ID attribue a la lampe lors de la mise en service
ENDPOINT = "1"         # endpoint de la lumiere

BROKER = "localhost"
PORT = 1883
TOPIC_SET = "home/light/set"
TOPIC_STATE = "home/light/state"
TOPIC_AVAIL = "home/light/availability"
POLL_INTERVAL = 30     # relecture periodique de l'etat (s)

commands = queue.Queue()   # commandes recues, traitees une par une
stop_event = threading.Event()


def log(msg):
    print(f"bridge: {msg}", flush=True)


def chip_tool(*args, timeout=30):
    """Lance chip-tool avec une LISTE d'arguments (jamais de shell).
    Retourne la sortie texte, ou None en cas d'echec."""
    cmd = [CHIP_TOOL, *args]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        log(f"timeout: {' '.join(args)}")
        return None
    if r.returncode != 0:
        log(f"chip-tool error ({r.returncode}): {' '.join(args)}")
        return None
    return r.stdout + r.stderr


def read_state():
    """Lit l'etat de la lampe : attributs OnOff et CurrentLevel."""
    state = {"on": None, "level": None}

    out = chip_tool("onoff", "read", "on-off", NODE_ID, ENDPOINT)
    m = re.search(r"OnOff: (TRUE|FALSE)", out or "")
    if m:
        state["on"] = (m.group(1) == "TRUE")

    out = chip_tool("levelcontrol", "read", "current-level", NODE_ID, ENDPOINT)
    m = re.search(r"CurrentLevel: (\d+)", out or "")
    if m:
        state["level"] = int(m.group(1))

    return state


def execute(payload):
    """Valide puis execute une commande recue en MQTT."""
    p = payload.strip().upper()

    if p in ("ON", "OFF", "TOGGLE"):
        return chip_tool("onoff", p.lower(), NODE_ID, ENDPOINT) is not None

    if p.isdigit() and 0 <= int(p) <= 254:
        # move-to-level <niveau> <transition> <optionsMask> <optionsOverride>
        return chip_tool("levelcontrol", "move-to-level", p, "0", "0", "0",
                         NODE_ID, ENDPOINT) is not None

    log(f"commande refusee: {payload!r}")
    return False


def worker(client):
    """Fil unique : traite les commandes et relit l'etat periodiquement.
    chip-tool n'est donc jamais lance deux fois en meme temps."""
    while not stop_event.is_set():
        try:
            payload = commands.get(timeout=POLL_INTERVAL)
            if payload is None:          # signal d'arret
                break
            log(f"commande: {payload}")
            if not execute(payload):
                continue
        except queue.Empty:
            pass                         # rien recu : relecture periodique

        state = read_state()
        log(f"etat: {state}")
        client.publish(TOPIC_STATE, json.dumps(state), qos=1, retain=True)


def on_connect(client, userdata, flags, rc):
    if rc != 0:
        log(f"connexion refusee: {mqtt.connack_string(rc)}")
        return
    log("connecte au broker")
    client.publish(TOPIC_AVAIL, "online", qos=1, retain=True)
    client.subscribe(TOPIC_SET, qos=1)


def on_message(client, userdata, msg):
    # Callback du fil reseau de paho : on ne bloque pas, on met en file.
    commands.put(msg.payload.decode("utf-8", errors="replace"))


def main():
    try:   # paho-mqtt 2.x
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION1,
                             client_id="matter-bridge")
    except AttributeError:   # paho-mqtt 1.x
        client = mqtt.Client(client_id="matter-bridge")

    client.on_connect = on_connect
    client.on_message = on_message
    client.will_set(TOPIC_AVAIL, "offline", qos=1, retain=True)
    client.reconnect_delay_set(min_delay=1, max_delay=30)

    client.connect(BROKER, PORT, keepalive=60)
    client.loop_start()      # fil reseau gere par paho (reconnexion incluse)

    try:
        worker(client)
    except KeyboardInterrupt:
        log("arret demande")
    finally:
        stop_event.set()
        client.publish(TOPIC_AVAIL, "offline", qos=1, retain=True).wait_for_publish(2)
        client.disconnect()
        client.loop_stop()


if __name__ == "__main__":
    main()
