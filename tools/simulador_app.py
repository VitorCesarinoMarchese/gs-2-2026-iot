"""Simulador do lado app/backend para demonstrar o chaveiro no Wokwi.

Faz o papel que o app real vai ter: escuta os eventos do chaveiro,
confirma o recebimento (ack) e mostra o que seria feito (bloquear o celular,
avisar a polícia e os contatos). Os contatos abaixo são fictícios.

Uso:
    pip install -r tools/requirements.txt
    python tools/simulador_app.py [--device chaveiro-0001] [--no-ack]

Comandos no terminal enquanto roda:
    find      faz o chaveiro piscar e tocar (achar as chaves)
    resolve   encerra o alerta ativo pelo app
    ping      pede o status atual do chaveiro
    sair
"""

import argparse
import json
import threading
from datetime import datetime, timezone

import paho.mqtt.client as mqtt

BROKER = "broker.hivemq.com"
PORT = 1883
TOPIC_ROOT = "citizentech/chaveiro"

# Dados simulados: nenhum dado pessoal real.
CONTATOS_FICTICIOS = [
    {"nome": "Contato 1 (simulado)", "canal": "SMS"},
    {"nome": "Contato 2 (simulado)", "canal": "WhatsApp"},
]

estado = {"alerta_ativo": None}


def agora() -> str:
    return datetime.now().strftime("%H:%M:%S")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--device", default="chaveiro-0001")
    parser.add_argument("--no-ack", action="store_true", help="não confirma eventos (testa o reenvio do chaveiro)")
    args = parser.parse_args()

    base = f"{TOPIC_ROOT}/{args.device}"
    topic_events, topic_status, topic_cmd = f"{base}/events", f"{base}/status", f"{base}/cmd"
    vistos: set[str] = set()

    def enviar(cmd: dict) -> None:
        client.publish(topic_cmd, json.dumps(cmd), qos=1)
        print(f"[{agora()}] -> chaveiro: {cmd}")

    def on_connect(client, userdata, flags, reason_code, properties):
        print(f"[{agora()}] Conectado ao broker {BROKER}. Ouvindo {base}/#")
        client.subscribe([(topic_events, 1), (topic_status, 1)])

    def on_message(client, userdata, msg):
        try:
            data = json.loads(msg.payload)
        except json.JSONDecodeError:
            print(f"[{agora()}] payload inválido em {msg.topic}")
            return

        if msg.topic == topic_status:
            online = "ONLINE" if data.get("online") else "OFFLINE"
            print(f"[{agora()}] status: {online} | bateria {data.get('battery', '?')}% | "
                  f"alerta ativo: {data.get('alertActive', False)}")
            return

        tipo, event_id = data.get("type"), data.get("eventId")
        repetido = event_id in vistos
        vistos.add(event_id)
        print(f"[{agora()}] <- evento {tipo} {event_id} (tentativa {data.get('attempt')}, "
              f"apertado há {data.get('ageMs')} ms)")

        if not repetido:  # o chaveiro reenvia até receber ack; a ação só roda uma vez
            if tipo == "alert":
                estado["alerta_ativo"] = data.get("alertId")
                ts = data.get("ts")
                quando = datetime.fromtimestamp(ts, timezone.utc).isoformat() if ts else "agora"
                print("   !!! ALERTA DE PÂNICO !!!")
                print("   1. Celular do usuário: comando de bloqueio enviado (lockNow)")
                print("   2. Localização: última posição do celular anexada ao alerta")
                print(f"   3. Polícia (190): ocorrência registrada, acionado em {quando}")
                for c in CONTATOS_FICTICIOS:
                    print(f"   4. {c['nome']}: aviso enviado por {c['canal']}")
            elif tipo == "cancel":
                estado["alerta_ativo"] = None
                print("   Usuário cancelou no chaveiro (estou bem). Contatos e polícia avisados do cancelamento.")
            elif tipo == "test":
                print("   Teste: cadeia chaveiro -> app funcionando. Ninguém foi acionado.")
            elif tipo == "low_battery":
                print(f"   Notificação no app: bateria do chaveiro em {data.get('battery')}%.")

        if tipo in ("alert", "cancel", "test") and not args.no_ack:
            enviar({"cmd": "ack", "eventId": event_id})

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.on_connect = on_connect
    client.on_message = on_message
    client.reconnect_delay_set(min_delay=1, max_delay=10)
    # O broker público às vezes recusa a primeira conexão; o loop tenta de novo sozinho.
    client.connect_async(BROKER, PORT, keepalive=30)
    threading.Thread(target=client.loop_forever, kwargs={"retry_first_connection": True}, daemon=True).start()

    print("Comandos" + __doc__.split("Comandos")[1])
    while True:
        try:
            linha = input().strip().lower()
        except (EOFError, KeyboardInterrupt):
            break
        if linha == "find":
            enviar({"cmd": "find"})
        elif linha == "ping":
            enviar({"cmd": "ping"})
        elif linha == "resolve":
            if estado["alerta_ativo"]:
                enviar({"cmd": "resolve", "alertId": estado["alerta_ativo"]})
                estado["alerta_ativo"] = None
            else:
                print("Nenhum alerta ativo.")
        elif linha in ("sair", "exit", "quit"):
            break
    client.disconnect()


if __name__ == "__main__":
    main()
