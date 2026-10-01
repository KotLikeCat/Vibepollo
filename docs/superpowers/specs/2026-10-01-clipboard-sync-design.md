# Синхронизация буфера обмена Moonlight (macOS) ↔ Vibepollo (Windows)

Дата: 2026-10-01. Статус: дизайн согласован, ждёт ревью спецификации.

## 1. Цель и рамки

Общий буфер обмена между клиентом Moonlight на macOS и хостом Vibepollo на Windows,
по ощущениям как в RDP: скопировал на одной стороне, вставил на другой, без горячих клавиш.

**Входит (этап 1, этот документ):** простой текст, HTML, RTF, картинки (включая скриншоты).

**Не входит:** файлы (CF_HDROP / `public.file-url`) — отдельный этап 2 со своей спецификацией;
клиенты кроме macOS; хосты кроме Windows; изменения веб-интерфейса Vibepollo.

**Критерии успеха:**
- Скриншот на Mac (Cmd+Ctrl+Shift+4) вставляется в Paint/Telegram на хосте сразу после входа в окно Moonlight.
- Текст/HTML/RTF, скопированные на хосте, вставляются на Mac сразу после выхода из окна Moonlight
  (включая Cmd+Tab → Cmd+V без паузы).
- Картинка, скопированная на хосте (Ножницы, браузер), вставляется на Mac после выхода из окна.
- Пароли из менеджеров паролей с Mac на хост не уходят.
- Стоковый Moonlight с новым хостом и новый клиент со стоковым Sunshine/Vibepollo работают как раньше.

## 2. Решения, принятые в обсуждении

| Вопрос | Решение |
|---|---|
| Файлы | Не в этом этапе (вариант C → позже A, «ленивые» как в RDP) |
| Момент синхронизации | Mac → хост только при получении фокуса окном Moonlight; хост → Mac сразу (вариант B) |
| Транспорт | Уведомление по управляющему каналу (ENet), данные по HTTPS (вариант 2) |
| Мелкие форматы хост → Mac | Текст/HTML/RTF забираются сразу по уведомлению (с debounce), картинки — при потере фокуса |

## 3. Затрагиваемые компоненты

1. **Vibepollo** (`KotLikeCat/Vibepollo`, ветка `feat/clipboard-sync`) — хост.
2. **moonlight-common-c** — новый форк `KotLikeCat/moonlight-common-c` (от `moonlight-stream/moonlight-common-c`),
   ветка `feat/clipboard-sync`. Используется **только клиентом**; Vibepollo свой pin (`Nonary/moonlight-common-c`) не меняет —
   хосту новый пакет нужен лишь как запись в собственной таблице `packetTypes` в `src/stream.cpp`.
3. **moonlight-qt** (`KotLikeCat/moonlight-qt`, ветка `feat/clipboard-sync`) — клиент; submodule
   `moonlight-common-c/moonlight-common-c` переключается на форк из п. 2.

## 4. Протокол

### 4.1 Объявление возможности

Хост добавляет в ответ `/serverinfo` (и HTTP, и HTTPS вариант) элемент:

```xml
<ClipboardSync>1</ClipboardSync>
```

Значение — версия протокола синхронизации (сейчас `1`). Элемент присутствует, только если на хосте
включена настройка `clipboard_sync`. Клиент включает функцию, только если версия `>= 1` и пользовательская
настройка включена. Клиенты без поддержки игнорируют неизвестный элемент.

### 4.2 Уведомление хост → клиент: пакет `0x3003`

- Тип управляющего пакета: `0x3003` («Clipboard changed», продолжение диапазона расширений Apollo `0x3000–0x3002`).
- Направление: только хост → клиент. Отправляется зашифрованным, как остальные пакеты (`encode_control`).
- Полезная нагрузка, little-endian, 8 байт:

| Смещение | Тип | Поле | Значение |
|---|---|---|---|
| 0 | u32 | `seq` | Номер версии буфера хоста (значение `GetClipboardSequenceNumber()`) |
| 4 | u32 | `formats` | Битовая маска доступных форматов, см. 4.4 |

- Пакет длиной payload `< 8` клиент отбрасывает. Лишние байты сверх 8 игнорируются (задел на расширение).
- Стоковый moonlight-common-c неизвестные типы молча освобождает — обратная совместимость сохраняется.

### 4.3 Данные: HTTPS `/actions/clipboard`, `type=bundle`

Существующие правила эндпоинта сохраняются без изменений: клиентский сертификат (exact enabled record),
права `_allow_view` + `clipboard_read` (GET) / `clipboard_set` (POST), клиент должен быть в активном стриме.
Режим `type=text` остаётся прежним (совместимость с Artemis).

**`GET /actions/clipboard?type=bundle`**
- `200` + тело — контейнер bundle (4.5) с текущим содержимым буфера хоста; заголовок `X-Clipboard-Seq: <u32>` —
  номер версии, из которой сделан снимок. Клиент запоминает именно его (а не `seq` из уведомления).
- Необязательный параметр `formats=<маска>` — запросить только указанные форматы (клиент использует его для
  «быстрого» запроса без картинки: `formats=7`). Без параметра — все поддерживаемые форматы.
- `204` — в буфере нет поддерживаемых форматов.
- `413` — даже после отбрасывания картинки содержимое больше `clipboard_max_bytes`.
- `503` — буфер временно недоступен (занят другой программой после всех повторов, экран блокировки/UAC).

**`POST /actions/clipboard?type=bundle`**, тело — контейнер bundle.
- `200` — записано (атомарно, все элементы одной транзакцией `OpenClipboard`/`EmptyClipboard`/`SetClipboardData`/`CloseClipboard`).
- `400` — некорректный контейнер. `413` — тело больше `clipboard_max_bytes`. `503` — буфер недоступен.
- Хост проверяет размер по `Content-Length` до разбора; кроме того, для HTTPS-сервера nvhttp задаётся
  `config.max_request_streambuf_size = clipboard_max_bytes + 64 КиБ` (сейчас лимита нет вовсе).

### 4.4 Форматы и битовая маска

| Тип элемента (u8) | Бит маски | Формат на проводе |
|---|---|---|
| 1 | `0x1` | Текст: UTF-8, переводы строк `\n` |
| 2 | `0x2` | HTML: фрагмент HTML в UTF-8 (без заголовка CF_HTML) |
| 3 | `0x4` | RTF: байты RTF как есть |
| 4 | `0x8` | Картинка: PNG |

Бит маски = `1 << (тип - 1)`. Другие типы/биты зарезервированы; неизвестные элементы при разборе пропускаются.

### 4.5 Контейнер bundle v1

Все числа little-endian.

```
offset  size  field
0       4     magic   = "MLCB" (0x4D 0x4C 0x43 0x42)
4       1     version = 1
5       1     count   (0..8)
6       ...   count элементов:
              u8   type    (см. 4.4)
              u32  length  (байт данных)
              u8[length] data
```

Правила разбора (обе стороны, общий набор тест-векторов):
- Неверный magic, `version != 1`, `count > 8`, выход за границы буфера, хвостовые байты после последнего
  элемента — ошибка (`400` на хосте, отказ на клиенте).
- Повторяющийся тип — ошибка.
- Элемент неизвестного типа — пропускается (но учитывается в проверке границ).
- Суммарный размер контейнера ограничен `clipboard_max_bytes` (по умолчанию 32 МиБ = 33554432).
- Порядок элементов при записи: текст, HTML, RTF, картинка.

## 5. Хост (Vibepollo, Windows)

### 5.1 Модули

| Файл | Назначение |
|---|---|
| `src/clipboard/bundle.{h,cpp}` | Кодек контейнера bundle (кроссплатформенный, без WinAPI) |
| `src/clipboard/sync_policy.{h,cpp}` | Чистая логика: кому слать уведомление, подавление эха, отбрасывание картинки по лимиту |
| `src/clipboard/watcher.{h,cpp}` | Поток-наблюдатель: опрос номера версии, запуск уведомлений |
| `src/platform/windows/clipboard_win.{h,cpp}` | Чтение/запись форматов Windows, CF_HTML, конвертация картинок через WIC |
| `src/platform/common.h` | Новые платформенные функции (см. 5.2); на macOS/Linux — заглушки «не поддерживается» |
| `src/nvhttp.cpp` | `type=bundle` в `getClipboard`/`setClipboard`, `<ClipboardSync>` в serverinfo, лимит тела запроса |
| `src/stream.cpp/.h` | `IDX_CLIPBOARD_CHANGED = 0x3003`, очередь уведомлений, отправка из потока control broadcast |
| `src/config.{h,cpp}`, `docs/configuration.md` | Настройки `clipboard_sync`, `clipboard_max_bytes` |

### 5.2 Платформенный интерфейс

```cpp
namespace platf::clipboard {
  enum class item_type : std::uint8_t { text = 1, html = 2, rtf = 3, png = 4 };
  struct item { item_type type; std::string data; };   // данные уже в «проводном» формате (4.4)

  std::uint32_t sequence();                 // GetClipboardSequenceNumber()
  std::uint32_t available_formats();        // маска 4.4 по IsClipboardFormatAvailable, без чтения данных
  std::optional<std::vector<item>> read(std::uint32_t formats_mask);   // nullopt = буфер недоступен
  std::optional<std::uint32_t> write(const std::vector<item> &items);  // номер версии после записи; nullopt = ошибка
}
```

### 5.3 Форматы Windows

- **Текст:** чтение `CF_UNICODETEXT` → UTF-8, `\r\n` → `\n`. Запись: `\n` → `\r\n` (существующий `ensureCrLf`) → `CF_UNICODETEXT`.
- **HTML:** формат `RegisterClipboardFormatW(L"HTML Format")`. Чтение: разбор заголовка (`StartFragment`/`EndFragment`
  — байтовые смещения в UTF-8), на провод — фрагмент. Запись: сборка заголовка `Version:0.9` с корректными
  `StartHTML/EndHTML/StartFragment/EndFragment` вокруг `<html><body><!--StartFragment-->…<!--EndFragment--></body></html>`.
- **RTF:** формат `RegisterClipboardFormatW(L"Rich Text Format")`, байты как есть (без завершающего `\0` на проводе;
  при записи `\0` добавляется).
- **Картинка — чтение:** приоритет `RegisterClipboardFormatW(L"PNG")` (байты как есть) → `CF_DIBV5` → `CF_DIB`;
  DIB конвертируется в PNG через WIC (`IWICImagingFactory`, `GUID_ContainerFormatPng`), с учётом альфы в DIBV5.
- **Картинка — запись:** `PNG` (байты как есть) + `CF_DIBV5` (PNG → декод WIC → 32-bit BGRA DIBV5),
  чтобы вставлялось и в Paint, и в приложения, читающие только DIB.
- `OpenClipboard` при неудаче повторяется до 5 раз с паузой 20 мс; затем — «недоступно».
- `sunshine.exe` работает в активной консольной сессии (запускается `sunshinesvc` через `CreateProcessAsUserW`),
  поэтому доступ к буферу пользователя есть. На экране блокировки/UAC операции тихо завершаются как «недоступно».

### 5.4 Наблюдатель и уведомления

- Поток-наблюдатель работает, только пока есть хотя бы одна активная stream-сессия; опрос `sequence()` раз в 250 мс.
- При изменении номера: `available_formats()`; если маска `0` — ничего не отправляем.
- Получатели уведомления — все активные сессии, у клиента которых есть `clipboard_read`,
  **кроме** клиента-автора, если эта версия появилась в результате его же `POST` (см. 5.5).
- ENet не потокобезопасен: наблюдатель **не** отправляет пакеты сам, а кладёт событие `{seq, formats, exclude_client_uuid}`
  в очередь, которую разбирает существующий `controlBroadcastThread` (так же, как сейчас отправляется HDR-режим);
  отправка — новая функция `send_clipboard_changed(session_t*, seq, formats)` по образцу `send_hdr_mode`.
- Сессия, у которой ещё нет `control.peer`, текущее уведомление пропускает. Когда у сессии впервые появляется
  `control.peer` (клиент подключился к управляющему каналу), в очередь ставится уведомление о **текущем** состоянии
  буфера только для этой сессии (если маска форматов не `0` и у клиента есть `clipboard_read`) — так клиент
  получает содержимое, скопированное на хосте до начала стрима.

### 5.5 Подавление эха

`sync_policy` хранит последнюю запись хоста: `{seq_after_write, origin_client_uuid}`.
Когда наблюдатель видит `seq == seq_after_write`, уведомление отправляется всем подходящим клиентам, кроме `origin_client_uuid`.
Любой другой `seq` — уведомление всем подходящим.

### 5.6 Лимит размера при чтении

После `read()`: если суммарный размер контейнера больше `clipboard_max_bytes`, удаляется элемент `png`;
если всё ещё больше — ответ `413` и запись в лог.

### 5.7 Настройки хоста

| Ключ | Тип | По умолчанию | Смысл |
|---|---|---|---|
| `clipboard_sync` | bool | `enabled` | Включает `<ClipboardSync>`, наблюдатель и `type=bundle` |
| `clipboard_max_bytes` | int | `33554432` | Лимит контейнера в байтах (минимум 1 МиБ, максимум 256 МиБ) |

В этом этапе — только конфиг-файл и `docs/configuration.md`; веб-интерфейс не трогаем.

## 6. moonlight-common-c (форк)

- `Limelight.h`: новый колбэк в **конце** `CONNECTION_LISTENER_CALLBACKS`:
  `typedef void(*ConnListenerClipboardChanged)(uint32_t seq, uint32_t formats);` поле `clipboardChanged`.
- `FakeCallbacks.c`: пустая реализация + подстановка в `fixupMissingCallbacks`.
- `ControlStream.c`: новый индекс `IDX_CLIPBOARD_CHANGED` со значением `0x3003` только в таблице для Sunshine
  (в остальных поколениях `-1`); обработка через существующую асинхронную очередь колбэков (`needsAsyncCallback`
  + разбор в очередь), проверка длины payload `>= 8`.

## 7. Клиент (moonlight-qt, macOS)

### 7.1 Модули

| Файл | Назначение |
|---|---|
| `app/streaming/clipboard/bundle.{h,cpp}` | Кодек bundle (тот же формат и те же тест-векторы, что на хосте) |
| `app/streaming/clipboard/syncstate.{h,cpp}` | Чистый класс состояния: решает, когда забирать/отправлять (без Qt-окон и AppKit) |
| `app/streaming/clipboard/macpasteboard.{h,mm}` | Нативный `NSPasteboard`: `changeCount`, чтение/запись форматов, признаки скрытых данных |
| `app/streaming/clipboard/clipboardsync.{h,cpp}` | Контроллер: связывает события сессии, колбэк, HTTP в фоне |
| `app/backend/nvhttp.{h,cpp}` | Методы получения тела как байтов + заголовков и `POST` с телом |
| `app/backend/nvcomputer.{h,cpp}` | Разбор `<ClipboardSync>` из serverinfo |
| `app/settings/streamingpreferences.{h,cpp}`, `app/gui/SettingsView.qml` | Настройка «Синхронизация буфера обмена», по умолчанию вкл |
| `app/streaming/session.{h,cpp}` | Подключение колбэка `clipboardChanged` и событий `FOCUS_GAINED`/`FOCUS_LOST` к контроллеру |
| `app/app.pro` | Новые файлы; (отдельным коммитом) `$$shell_quote` для `Info.plist` — сборка из пути с пробелами |

### 7.2 Форматы macOS

- Чтение (для POST): `public.utf8-plain-text` → тип 1; `public.html` → 2; `public.rtf` → 3;
  картинка: `public.png` как есть, иначе `public.tiff` → PNG через `NSBitmapImageRep` → 4.
- Запись (после GET): `clearContents` + `declareTypes` + запись каждого полученного элемента в соответствующий тип;
  PNG записывается как `public.png`.
- **Никогда не отправлять**, если на доске есть любой из типов: `org.nspasteboard.ConcealedType`,
  `org.nspasteboard.TransientType`, `org.nspasteboard.AutoGeneratedType`.

### 7.3 Состояние и правила (`syncstate`)

Состояние на одну стрим-сессию: `pendingHostSeq`, `pendingHostFormats` (из последнего уведомления),
`appliedHostSeq`, `appliedHostFormats` (версия и маска того, что уже записано в буфер Mac),
`lastSentMacChangeCount`, `ownMacChangeCount` (значение `changeCount` после нашей записи), `focused`.

«Не хватает данных хоста» (`hostDataMissing`) = `pendingHostSeq != appliedHostSeq`
**или** `(pendingHostFormats & ~appliedHostFormats) != 0`.

| Событие | Действие |
|---|---|
| `clipboardChanged(seq, formats)` | запомнить в `pending*`; если окно не в фокусе — сразу полный `GET`; иначе, если в `formats` есть биты `0x7`, через 300 мс debounce «быстрый» `GET ?type=bundle&formats=7` |
| Потеря фокуса | если `hostDataMissing` — полный `GET` (все форматы) |
| GET успешен | записать полученные элементы в `NSPasteboard`, `ownMacChangeCount = changeCount`, `appliedHostSeq = X-Clipboard-Seq`, `appliedHostFormats` = маска реально полученных элементов |
| Получение фокуса (включая первое за сессию) | если `changeCount != lastSentMacChangeCount` и `!= ownMacChangeCount` и нет скрытых типов — `POST`; при успехе `lastSentMacChangeCount = changeCount` |

- Пример (Excel): уведомление с маской `0xF` → быстрый GET приносит текст/HTML/RTF (`appliedHostFormats = 0x7`) →
  при потере фокуса `hostDataMissing` истинно (нет бита `0x8`) → полный GET дописывает картинку.
- Номера версий сравниваются только на равенство (u32 может переполниться). Если буфер хоста сменился между
  уведомлением и GET, `X-Clipboard-Seq` будет новее, а следующее уведомление с этим же `seq` уже в пути —
  оно лишь обновит `pending*`, и `hostDataMissing` решит, нужен ли ещё запрос.
- Все HTTP-запросы — в отдельном рабочем потоке; таймаут 5 с для запросов без картинки, 20 с с картинкой.
  Одновременно не больше одного GET и одного POST; новые события, пришедшие во время запроса, обрабатываются после него.
- Ошибки — только в лог (`SDL_Log`), без диалогов; повтор — при следующей смене фокуса/уведомлении.
- Функция активна, только если: настройка клиента включена, хост объявил `ClipboardSync >= 1`.
  Права клиента не проверяются заранее: `401/403` → функция отключается до конца сессии с записью в лог.

## 8. Совместимость

- Стоковый Moonlight + новый хост: пакет `0x3003` игнорируется, `type=bundle` не используется.
- Новый клиент + стоковый Sunshine / Vibepollo без функции: нет `<ClipboardSync>` → функция выключена.
- Artemis + новый хост: продолжает пользоваться `type=text`.
- Существующее сочетание «напечатать буфер» (Ctrl+Alt+Shift+V) в moonlight-qt не меняется.

## 9. Безопасность

- Данные — только по HTTPS с проверкой клиентского сертификата (exact enabled record), уведомления — в
  зашифрованном управляющем канале.
- Права `clipboard_read`/`clipboard_set` на клиента — как сейчас (веб-панель Vibepollo).
- Лимит размера тела запроса закрывает отсутствующий сейчас лимит HTTPS-сервера nvhttp.
- Mac → хост: только при фокусе окна Moonlight и без скрытых/временных типов (пароли).
- Содержимое буфера не пишется в лог (только типы и размеры).

## 10. Тестирование

**Автотесты хоста** (gtest, `tests/unit/`): кодек bundle (общие тест-векторы: корректный, пустой, неизвестный тип,
дубликат типа, обрезанный, хвостовые байты, `count > 8`, превышение лимита); CF_HTML разбор/сборка (ASCII,
многобайтный UTF-8, отсутствие маркеров); `\r\n` ↔ `\n`; `sync_policy` (эхо, несколько клиентов, клиент без
`clipboard_read`, отбрасывание картинки по лимиту). Наш workflow `build-windows-selfhosted.yml` собирает и
запускает только эти тесты (фильтр gtest), не весь набор.

**Автотесты клиента** (QtTest, локально на Mac): кодек bundle на тех же тест-векторах (файлы векторов лежат в
Vibepollo `tests/fixtures/clipboard_bundle/` и копируются в тест клиента); `syncstate` (все строки таблицы 7.3,
debounce, повторы, эхо).

**Ручная проверка** (хост `win11-gaming`, клиент — Mac пользователя): текст в обе стороны (кириллица, эмодзи);
HTML браузер (Mac) → Word (Windows); RTF Word → TextEdit; скриншот Mac → Paint/Telegram; Ножницы → Просмотр/Telegram;
диапазон Excel; пароль из менеджера паролей не уходит; серия быстрых копирований; экран блокировки Windows;
картинка > 32 МиБ (текст доходит, картинка нет); совместимость из раздела 8.
Установка тестовой сборки на `win11-gaming` — только с отдельного разрешения пользователя.

## 11. Этапы реализации

1. moonlight-common-c: форк, пакет `0x3003`, колбэк; moonlight-qt переключает submodule.
2. Хост, данные: bundle-кодек + тесты, `clipboard_win` (форматы, CF_HTML, WIC), `type=bundle`, `<ClipboardSync>`, настройки, лимит тела.
3. Хост, уведомления: наблюдатель, очередь в control broadcast, `send_clipboard_changed`, `sync_policy` + тесты.
4. Клиент: bundle-кодек + тесты, `macpasteboard`, POST/байты в `NvHTTP`, `syncstate` + тесты, контроллер, настройка; правка `app.pro`.
5. Ручная проверка по разделу 10, документация (`docs/configuration.md`, README форков).
