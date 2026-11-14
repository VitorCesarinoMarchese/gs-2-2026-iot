# Contrato MQTT do chaveiro

Este documento é para quem faz o app e o backend. Ele descreve tudo que o chaveiro publica e tudo que ele entende. Se o app seguir isto, as partes se encaixam.

## Conexão

| Item | Valor (protótipo) |
|---|---|
| Broker | `broker.hivemq.com` (público, só para demonstração) |
| Porta | `1883` (TCP) ou `8884` (WebSocket com TLS, para app/navegador) |
| Raiz dos tópicos | `citizentech/chaveiro/<deviceId>/` |
| `deviceId` no Wokwi | `chaveiro-0001` |

No produto real, o `deviceId` vem impresso num QR code no chaveiro. O app lê o QR para parear o chaveiro com a conta do usuário. O chaveiro não guarda nem envia nenhum dado pessoal: quem sabe que `chaveiro-0001` pertence à Maria, e quem são os contatos dela, é o backend.

## Tópicos

| Tópico | Direção | Retido | Para quê |
|---|---|---|---|
| `.../events` | chaveiro → app | não | alerta, cancelamento, teste, bateria baixa |
| `.../status` | chaveiro → app | sim | online/offline, bateria, sinal, alerta ativo |
| `.../cmd` | app → chaveiro | não | confirmar recebimento, encerrar alerta, achar chaveiro |

## Eventos (`.../events`)

Todos têm o mesmo formato:

```json
{
  "eventId": "chaveiro-0001-6737a1f0-17",
  "type": "alert",
  "deviceId": "chaveiro-0001",
  "alertId": "chaveiro-0001-6737a1f0-17",
  "ts": 1763089200,
  "ageMs": 140,
  "attempt": 1,
  "battery": 82,
  "fw": "1.0.0"
}
```

| Campo | Significado |
|---|---|
| `eventId` | único por evento, também entre reinícios e entre execuções do simulador (o meio do id muda a cada boot). Trate como texto opaco e use para descartar repetidos. |
| `type` | `alert`, `cancel`, `test` ou `low_battery` |
| `alertId` | em `alert` é igual ao `eventId`; em `cancel` diz qual alerta foi cancelado (pode ser um que nunca chegou ao app; mesmo assim, o cancelamento fecha todos os alertas ativos do chaveiro). |
| `ts` | hora do clique em segundos Unix (UTC). Vem `null` se o chaveiro ainda não sincronizou o relógio. |
| `ageMs` | há quantos milissegundos o botão foi apertado. Com `ts: null`, a hora do clique é `hora de chegada - ageMs`. Exceção: num alerta reenviado depois de um reinício, `ageMs` conta a partir do reinício; prefira `ts` quando vier preenchido. |
| `attempt` | número da tentativa de envio (1, 2, 3...) |
| `battery` | bateria em % |

### Quando cada evento sai

| `type` | Gesto | Precisa de `ack`? | O que o app/backend faz |
|---|---|---|---|
| `alert` | 1 clique (soltar antes de 3 s também conta como clique) | sim | bloqueia o celular, anexa a última localização do celular, aciona a polícia e os contatos |
| `cancel` | segurar 3 s com alerta ativo | sim | avisa a polícia e os contatos que o usuário está bem. Fecha todos os alertas ativos do chaveiro, não só o `alertId` indicado. |
| `test` | segurar 3 s sem alerta | sim | só mostra "chaveiro funcionando" no app. Ninguém é acionado. |
| `low_battery` | bateria ≤ 15% | não | notificação "carregue seu chaveiro" |

### Reenvio e repetidos

Eventos que precisam de `ack` são reenviados a cada 5 s até o app responder. Se o chaveiro está sem rede, o evento espera e sai quando a conexão voltar. Um alerta sem confirmação também sobrevive a um reinício do chaveiro.

O chaveiro sempre envia os eventos pendentes na ordem em que aconteceram, e um alerta novo só sai depois dos eventos antigos ainda sem confirmação. Processe os eventos na ordem de chegada.

Por isso o app **vai receber o mesmo `eventId` mais de uma vez**. A regra é: executar a ação só na primeira vez e mandar `ack` todas as vezes.

Se um alerta já confirmado continua aberto (ninguém mandou `resolve`) e o usuário clica de novo, o chaveiro abre **outro** alerta, com `alertId` novo. É uma nova emergência.

## Status (`.../status`, retido)

```json
{
  "deviceId": "chaveiro-0001",
  "online": true,
  "battery": 82,
  "lowBattery": false,
  "rssi": -61,
  "alertActive": false,
  "alertId": null,
  "uptimeS": 3600,
  "fw": "1.0.0"
}
```

Sai ao conectar, a cada 30 s, logo depois de um alerta abrir ou fechar, e quando o app manda `ping`. Se o chaveiro cair, o broker publica a "última vontade" `{"deviceId": "chaveiro-0001", "online": false}` no mesmo tópico. Como a mensagem é retida, o app recebe o último estado assim que se inscreve.

## Comandos (`.../cmd`)

| Comando | Exemplo | Efeito no chaveiro |
|---|---|---|
| `ack` | `{"cmd":"ack","eventId":"chaveiro-0001-6737a1f0-17"}` | para o reenvio. Em alerta: LED vermelho fixo por 3 s ("ajuda a caminho"). Em teste: LED verde e um bipe (sem bipe se houver alerta ativo). |
| `resolve` | `{"cmd":"resolve","alertId":"chaveiro-0001-6737a1f0-17"}` | encerra o alerta (o usuário cancelou pelo app ou a ocorrência foi fechada) |
| `find` | `{"cmd":"find"}` | pisca e toca por 6 s para achar as chaves. Ignorado com alerta ativo: o chaveiro nunca faz som durante um alerta. |
| `ping` | `{"cmd":"ping"}` | publica o status na hora |

## Testar sem o app

Com o Wokwi rodando, em outro terminal:

```bash
pip install -r tools/requirements.txt
python tools/simulador_app.py
```

Ou pelo navegador, no cliente web da HiveMQ (<https://www.hivemq.com/demos/websocket-client/>): conecte, inscreva-se em `citizentech/chaveiro/chaveiro-0001/#` e publique comandos em `citizentech/chaveiro/chaveiro-0001/cmd`.

## Segurança: o que muda fora do protótipo

O broker público serve só para a demonstração: qualquer pessoa que souber o tópico pode ler e publicar nele. No produto:

- broker próprio com TLS (porta 8883) e usuário/senha ou certificado por chaveiro;
- regras de acesso (ACL): cada chaveiro só publica nos próprios tópicos, e só o backend publica em `cmd`;
- o payload continua sem dados pessoais, então mesmo um vazamento do tópico não expõe quem é o usuário.
