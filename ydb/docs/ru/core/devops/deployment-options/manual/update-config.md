# Обновление конфигурации кластеров {{ ydb-short-name }}, развёрнутых вручную

При ручном развертывании кластера {{ ydb-short-name }} управление конфигурацией осуществляется через [YDB CLI](../../../reference/ydb-cli/index.md). В этой статье рассматриваются способы изменения конфигурации кластера после первоначального развертывания.

## Базовые операции с конфигурацией

### Получение текущей конфигурации

Для получения текущей конфигурации кластера используется команда:

```bash
ydb -e grpcs://<endpoint>:2135 admin cluster config fetch > config.yaml
```

В качестве `<endpoint>` указывается адрес любого из узлов кластера.

### Применение новой конфигурации

Для загрузки обновленной конфигурации на кластер используется следующая команда:

```bash
ydb -e grpcs://<endpoint>:2135 admin cluster config replace -f config.yaml
```

Некоторые параметры конфигурации применяются на ходу после выполнения команды, однако для некоторых требуется выполнение процедуры [перезапуска кластера](../../../maintenance/manual/node_restarting.md).