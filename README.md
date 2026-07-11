# NewPlayers + Discord Core

Плагин сохраняет первого подключившегося игрока в таблицу `new_players` и отправляет уведомление в Discord при каждом подключении реального игрока.

## Зависимости

- Metamod:Source 2
- [Utils](https://github.com/Pisex/cs2-menus)
- SQLMM с MySQL
- [cs2-discord_core py Pisex](https://github.com/Pisex/cs2-discord_core)

## Discord

Настройте файл:

`addons/configs/new_players/discord.ini`

Можно использовать один из вариантов:

1. `webhook` — URL webhook Discord-канала.
2. `bot_token` + `channel_id` — токен Discord-бота и ID канала.

Если заполнены оба варианта, используется webhook.

Название сервера автоматически берётся из серверной переменной `hostname`; указывать его в `discord.ini` не нужно.

## База данных

В `addons/configs/databases.cfg` должна быть секция:

```text
"new_players"
{
    "host"      "127.0.0.1"
    "user"      "root"
    "pass"      "password"
    "database"  "database_name"
    "port"      "3306"
}
```
