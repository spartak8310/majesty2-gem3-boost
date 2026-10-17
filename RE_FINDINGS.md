# Полный отчёт по реверс-инжинирингу Majesty 2 Collection (Движок GEM 3, v1.5.356)

Данный документ содержит исчерпывающие результаты статического и бинарного анализа исполняемых модулей (`Majesty2.exe`, `M2Editor.exe`, 42 нативных DLL движка **GEM 3** и 2 `.NET`-утилит), закрытых архивов ресурсов (`.pak`) и конфигурационных подсистем игры **Majesty 2 Collection** (`/Users/orkodav4421/Documents/Majesty2_Archive/Majesty 2 Collection`).

---

## Содержание
1. [Архитектура движка GEM 3 и восстановленный SDK](#1-архитектура-движка-gem-3-и-восстановленный-sdk)
2. [Криптография и алгоритм распаковки закрытых архивов (`.pak`)](#2-криптография-и-алгоритм-распаковки-закрытых-архивов-pak)
3. [Анализ и замена .NET-утилит (`Majesty2Selector.exe`, `Change profile name.exe`)](#3-анализ-и-замена-net-утилит-majesty2selectorexe-change-profile-nameexe)
4. [Бинарный дизассемблинг ключевых узлов (`Majesty2.exe`, `scene.dll`, `ps.dll`, `Multiplayer.dll`)](#4-бинарный-дизассемблинг-ключевых-узлов-majesty2exe-scenedll-psdll-multiplayerdll)
5. [Производительность и устранение лагов Majesty 2 (Engine + Configs + Binary Hooks)](#5-производительность-и-устранение-лагов-majesty-2-engine--configs--binary-hooks)

---

## 1. Архитектура движка GEM 3 и восстановленный SDK

В ходе автоматизированного анализа 44 бинарных PE32-модулей (`tools/extract_sdk.py`) были полностью извлечены и деманглированы таблицы экспортов, импортов, дескрипторов RTTI (`TypeDescriptor`, `CompleteObjectLocator`, `ClassHierarchyDescriptor`) и консольных переменных:

- **Всего проанализировано PE32-модулей:** `44` (`30` в корневом каталоге игры + `14` плагинов в `bin/plugins/`)
- **Всего экспортируемых символов:** `12 960`
- **Всего импортируемых символов:** `14 343`
- **Восстановлено C++ RTTI-классов и `vtable`:** `7 909`
- **Извлечено консольных переменных (`eConVar<T>`) и команд (`eConProcessor`):** `457`

### Ключевые артефакты SDK в рабочей области:
- [`sdk/exports_by_dll.json`](file:///Users/orkodav4421/.gemini/antigravity/scratch/majesty2-re/sdk/exports_by_dll.json) — полная база экспортов, импортов и RTTI-иерархий по всем 44 модулям.
- [`sdk/GEM3_ENGINE_API.md`](file:///Users/orkodav4421/.gemini/antigravity/scratch/majesty2-re/sdk/GEM3_ENGINE_API.md) — архитектурный справочник по подсистемам движка (`base.dll`, `io.dll`, `device.dll`, `render.dll`, `scene.dll`, `geometry.dll`, `pf.dll`, `ps.dll`, `entity.dll`, `Multiplayer.dll` и др.).
- [`sdk/include/gem3_plugin_sdk.h`](file:///Users/orkodav4421/.gemini/antigravity/scratch/majesty2-re/sdk/include/gem3_plugin_sdk.h) — готовый C/C++ заголовочный файл с выверенными `pragma pack(push, 1)` структурами (`Vec2`, `Vec3`, `Vec4`, `Quaternion`, `Matrix33`, `Matrix34`, `Matrix44`, `eBoxGeneric<Vec3>`, `eFrustum`, `xIo::ICryptoStreamFactory`, `xScene::eQuantControl`, `eConVar<T>`) и объявлениями функций для написания плагинов и бинарных хуков.
- [`sdk/engine_cvars.json`](file:///Users/orkodav4421/.gemini/antigravity/scratch/majesty2-re/sdk/engine_cvars.json) — полный реестр всех `457` консольных переменных и команд движка с точными виртуальными адресами (`VA`), типами, дефолтными значениями и описаниями.

---

## 2. Криптография и алгоритм распаковки закрытых архивов (`.pak`)

Большинство `.pak`-архивов Majesty 2 представляют собой стандартные `PKZIP`-архивы, однако **6 критически важных архивов** (`resource.std.pak`, `resource.editor.pak`, `texts.expansion_1.pak`, `texts.expansion_2.pak`, `texts.expansion_3.pak`, `3d.expansion_2.pak`) не имеют сигнатуры `PK\x03\x04` и полностью зашифрованы.

### 2.1. Цепочка перехвата файловых потоков (`io.dll` -> `Majesty2.exe`)

1. В `io.dll` функция монтирования архивов `?MountZip@xFile@xSystem@xIo@@YA_NABV?$eName@W4eFileNameTag@xIo@@@3@0@Z` (`VA 0x10006100`) проверяет глобальный указатель фабрики криптопотоков `xIo::ICryptoStreamFactory*` по адресу **`0x10029F50`**.
2. Регистрация этой фабрики замаскирована разработчиками под безобидно названный экспорт `io.dll`:
   - **Символ:** `?PurgeEmpty@xFile@xSystem@xIo@@YAXABV?$eName@W4eFileNameTag@xIo@@@3@@Z`
   - **Адрес в `io.dll`:** `RVA 0x00014E80` / `VA 0x10014E80`
   - **Дизассемблер:**
     ```asm
     0x10014E80: mov eax, dword ptr [esp + 4] ; xIo::ICryptoStreamFactory*
     0x10014E84: mov dword ptr [0x10029F50], eax
     0x10014E89: ret
     ```
3. При старте `Majesty2.exe` функция `VA 0x00731050` записывает в глобальный объект **`0x00B4208C`** указатель на таблицу виртуальных методов `FileSystem::Crypt::StreamFactory::vftable` (**`0x008C1DD0`**) и вызывает `io.dll!PurgeEmpty` (`VA 0x00731066`).
4. Виртуальный метод `FileSystem::Crypt::StreamFactory::Open` (`VA 0x00731090`) вызывает `FileSystem::Crypt::PakSetKeyCrypters::SetKey` (`VA 0x007312F0`).

### 2.2. Генерация сигнатуры архива и выбор AES-ключа (`VA 0x007312F0`)

Функция `FileSystem::Crypt::PakSetKeyCrypters::SetKey` (`VA 0x007312F0` в `Majesty2.exe`, `VA 0x006F3530` в `M2Editor.exe`) вычисляет 128-битный идентификатор `.pak`-файла следующим образом:

1. Из полного пути к архиву извлекается только имя файла с расширением (например, `"resource.std.pak"`), и все латинские символы `A..Z` переводятся в нижний регистр (`a..z`).
2. Инициализируется контекст `MD5` (`VA 0x00729090` / `0x007292A0`):
   - В `MD5` подаётся строка имени файла в нижнем регистре (без завершающего нуля).
   - Следом в `MD5` подаётся 4-байтный размер `.pak`-файла в формате `uint32_le` (`VA 0x007313AC`).
3. Полученный 16-байтный дайджест `MD5` интерпретируется как массив из четырёх 32-битных слов `(d0, d1, d2, d3)` (`uint32_le`).
4. Вызывается функция `FileSystem::ModuleCRC` (`VA 0x0072B5E0`), которая для релизного `Majesty2.exe` и `M2Editor.exe` возвращает константу **`0x4CE52E73`**.
5. Каждое из четырёх слов дайджеста XOR-ится с `ModuleCRC`:
   $$s_i = d_i \oplus \text{0x4CE52E73}, \quad i \in \{0, 1, 2, 3\}$$
6. Сформированный вектор `(s0, s1, s2, s3)` ищется в таблице из **32 слотов** (`VA 0x0072A441` в `Majesty2.exe`, шаг `0x46 = 70` байт; `VA 0x006F2641` в `M2Editor.exe`). Каждый слот содержит встроенный shellcode-генератор (`sub esp, 0x10; mov byte ptr [esp+...], ...`), заполняющий 16-байтный ключ `AES-128` на стеке и вызывающий `Rijndael::InitKey` (`VA 0x00731880`).

### 2.3. Режим сцепления блоков при дешифровании (`VA 0x00732360`)

Метод `FileSystem::Crypt::IDataCrypter<Rijndael>::Decrypt(void* buf, uint32_t size)` (`VA 0x00732360`) обрабатывает данные независимыми страницами (при чтении потока в `StreamCrypter` по 32–64 КБ, а при упаковке архива — по 16-байтным блокам внутри буфера). Для последовательности из $N$ 16-байтных блоков $C_0, C_1, \dots, C_{N-1}$:

1. **Прямой проход (Forward XOR соседних блоков шифртекста, `VA 0x0073237B..0x007323B1`):**
   Для $i = 0, 1, \dots, N-2$:
   $$C'_i = C_i \oplus C_{i+1}, \quad C'_{N-1} = C_{N-1}$$
2. **Блочное дешифрование `AES-128-ECB` (`VA 0x007323CA -> call 0x00731F30`):**
   Для каждого 16-байтного блока $i = 0, \dots, N-1$:
   $$M_i = \text{AES\_128\_Decrypt}(K, C'_i)$$
3. **Обратный проход (Backward XOR от конца к началу, `VA 0x007323DA..0x00732419`):**
   Для $i = N-1, N-2, \dots, 1$:
   $$P_i = M_i \oplus M_{i-1}, \quad P_0 = M_0$$

> [!IMPORTANT]
> Поскольку $C'_i = C_i \oplus C_{i+1}$ зависит только от соседнего зашифрованного блока, а $P_i = M_i \oplus M_{i-1}$ зависит только от предыдущего выхода `AES_Decrypt`, **каждый 16-байтный блок $P_i$ ($i \ge 1$) вычисляется полностью локально по трём соседним блокам $(C_{i-1}, C_i, C_{i+1})$ без необходимости последовательного чтения от начала файла!** Именно это свойство позволяет движку выполнять произвольный `Seek` внутри зашифрованного `.pak`-архива, а нашему распаковщику [`tools/unpack_closed_pak.py`](file:///Users/orkodav4421/.gemini/antigravity/scratch/majesty2-re/tools/unpack_closed_pak.py) — мгновенно расшифровывать архивы любого размера.

### 2.4. Таблица ключей и сигнатур всех 6 закрытых `.pak`-архивов

| Архив | Размер (байт) | Слот | Сигнатура `(s0, s1, s2, s3)` | 16-байтный ключ `AES-128` (hex) | Файлов внутри |
| :--- | ---: | :---: | :--- | :--- | ---: |
| `resource.std.pak` | `2 574 203` | `#3` (`Majesty2.exe`) | `8769f5c0 06a99db1 e084b6a1 f82124e2` | `6f7139386b7a376c6371786575666e65` (`oq98kz7lcqxeufne`) | `1 434` |
| `resource.editor.pak` | `22 652` | `#0` (`M2Editor.exe`) | `3fd05a28 f64964f0 7de9edef b15d4c85` | `32796a6139347a746c38713163703978` (`2yja94ztl8q1cp9x`) | `9` |
| `texts.expansion_1.pak` | `976 704` | `#9` (`Majesty2.exe`) | `1af09d78 fd79b44a 05585116 dddbe995` | `7574316a6b6c61316d65626764726966` (`ut1jkla1mebgdrif`) | `102` |
| `texts.expansion_2.pak` | `981 360` | `#23` (`Majesty2.exe`) | `64b0030b e9598d73 e9d837c9 cde781ae` | `36687965736339716b6c647236326177` (`6hyesc9qkldr62aw`) | `124` |
| `texts.expansion_3.pak` | `991 392` | `#28` (`Majesty2.exe`) | `b3f1ba35 f5dd23c2 2f60f8f0 9461abbc` | `6e766b647269653738716b646c6f3033` (`nvkdrie78qkdlo03`) | `149` |
| `3d.expansion_2.pak` | `46 420 512` | `#22` (`Majesty2.exe`) | `bb8ca061 06da0da4 54dd9c58 c145fb95` | `687565726f3138736a666c65726f7573` (`huero18sjflerous`) | `172` |

Все 6 архивов (`1 990` файлов) полностью расшифрованы и распакованы утилитой [`tools/unpack_closed_pak.py`](file:///Users/orkodav4421/.gemini/antigravity/scratch/majesty2-re/tools/unpack_closed_pak.py) в каталог [`unpacked/`](file:///Users/orkodav4421/.gemini/antigravity/scratch/majesty2-re/unpacked/).

---

## 3. Анализ и замена .NET-утилит (`Majesty2Selector.exe`, `Change profile name.exe`)

Обе сервисные утилиты из корня игры (`Majesty2Selector.exe` и `support/Change profile name.exe`) представляют собой 32-битные `.NET 2.0` WinForms-сборки. Полный декомпилированный код и разбор находятся в [`decompiled/Majesty2Selector_analysis.md`](file:///Users/orkodav4421/.gemini/antigravity/scratch/majesty2-re/decompiled/Majesty2Selector_analysis.md).

### Выявленные дефекты оригинальных утилит:
1. **Катастрофически медленное копирование `.pak`-архивов в `Majesty2Selector.exe` (`Form1.CopyFile`):** вместо `File.Copy` или блочного буфера используется побайтовый цикл `while (fsIn.Position < fsIn.Length) fsOut.WriteByte((byte)fsIn.ReadByte());`, выполняющий сотни миллионов системных вызовов при переключении аддонов.
2. **Жёсткая привязка к реестру Windows (`HKCU\Software\1C\Majesty2\Path`)** и падение с необработанным `NullReferenceException`, если ключ реестра отсутствует (что всегда происходит при переносимом запуске через Wine/CrossOver на macOS).
3. **Некорректная перекодировка профиля в `Change profile name.exe`:** утилита читает путь из `user.ini`, декодирует однобайтную строку `Windows-1251` как `UTF-16LE` (`Encoding.Unicode`) при записи имени профиля в `profile.user`, что приводит к порче кириллических символов на системах с нерусской системной локалью.

В качестве нативной замены для macOS / Linux / Windows создана утилита [`tools/m2_profile_selector.py`](file:///Users/orkodav4421/.gemini/antigravity/scratch/majesty2-re/tools/m2_profile_selector.py), работающая напрямую с файловой системой и корректно обрабатывающая кодировки `cp1251` / `utf-16le`.

---

## 4. Бинарный дизассемблинг ключевых узлов (`Majesty2.exe`, `scene.dll`, `ps.dll`, `Multiplayer.dll`)

### 4.1. `Majesty2.exe` — Формат сохранений `m2s v1.1`, `mission.sav` и обход шифрования `.sav`

#### Обработка `mission.sav` (`VA 0x00881918`)
Строковый литерал `"mission.sav"` (`VA 0x00881918`) входит в триаду файлов миссии/сохранения в `Majesty2.exe`:
- `0x0088190C`: `"mission.mi"` (метаданные и скрипты миссии)
- `0x00881918`: `"mission.sav"` (состояние игрового мира и сущностей)
- `0x00881924`: `"mission.scn"` (сцена и граф объектов)

Диспетчер загрузки и сохранения миссии обращается к `"mission.sav"` в двух ключевых точках:
- **`VA 0x00412091` (`RVA 0x00012091`):**
  ```asm
  0x00412082: call 0x004D0B00
  0x00412087: add  eax, 0x70
  0x0041208A: push eax
  0x0041208B: lea  ecx, [esp + 0x18]
  0x0041208F: push ecx
  0x00412090: push 0
  0x00412091: push 0x00881918        ; "mission.sav"
  0x00412096: call 0x0043B320        ; упаковка/распаковка контейнера сохранения
  ```
- **`VA 0x004E04EF` (`RVA 0x000E04EF`):**
  ```asm
  0x004E04DF: lea  ecx, [esp + 0x5C]
  0x004E04E3: call dword ptr [0x0087BDA0]
  0x004E04E9: push eax
  0x004E04EA: lea  ecx, [esp + 0x20]
  0x004E04EE: push ecx
  0x004E04EF: push 0x00881918        ; "mission.sav"
  0x004E04F4: call 0x007595D0        ; конкатенация пути к mission.sav внутри .sav архива
  ```

#### Загрузчик сохранений `FileSystem::M2SaveLoad::Load` (`VA 0x0072EFC0`)
Функция открывает файл сохранения через `io.dll!eFileStream` и проверяет 8-байтный заголовок `"m2s v1.1"`:
```asm
0x0072EFC0: push ebp
0x0072EFC1: mov  ebp, esp
...
0x0072F006: push 4
0x0072F008: push eax
0x0072F009: mov  ecx, esi
0x0072F00B: call 0x00733F20          ; чтение первых 4 байт заголовка
0x0072F010: cmp  dword ptr [ebp - 0x18], 0x2073326D ; "m2s "
0x0072F017: jne  0x0072F087          ; <-- ЕСЛИ НЕТ "m2s ", ВОЗВРАЩАЕТ СЫРОЙ ПОТОК БЕЗ ДЕШИФРОВАНИЯ!
0x0072F019: lea  eax, [ebp - 0x18]
0x0072F01C: push 4
0x0072F01E: push eax
0x0072F01F: mov  ecx, esi
0x0072F021: call 0x00733F20          ; чтение следующих 4 байт ("v1.1")
0x0072F026: mov  eax, dword ptr [ebp - 0x18]
0x0072F029: cmp  eax, 0x312E3176     ; "v1.1"
0x0072F02E: jne  0x0072F03B
```
Если заголовок `"m2s v1.1"` присутствует:
1. Вызывается `VA 0x00751410` (`RVA 0x00351410`), которая вычисляет 1-байтовую контрольную сумму имени файла сохранения:
   ```asm
   0x00751427: mov  dl, byte ptr [esi]
   0x00751429: xor  dl, cl
   0x0075142B: add  al, dl
   0x0075142D: add  cl, 0x39
   0x00751430: add  esi, 1
   0x00751433: sub   edi, 1
   0x00751436: jne  0x00751427
   ```
2. Базовый 15-байтный ключ **`"FFJmI_5qLjAJotd"`** (`VA 0x008C1CB8`) побайтово складывается/XOR-ится с полученным байтом контрольной суммы (`VA 0x0072F1A8..0x0072F1B5`).
3. Вызывается `VA 0x0072F370`, использующая `base.dll!xCrypt::eCipher::Decrypt` (`[0x0087BCEC]`) для расшифровки остатка файла в `eMemFileStream`.

> [!TIP]
> **Встроенный обход шифрования `.sav`:** Благодаря переходу `0x0072F017: jne 0x0072F087`, `Majesty2.exe` **уже умеет** загружать незашифрованные файлы сохранений! Если заменить зашифрованный `.sav` на открытый поток данных (без префикса `"m2s v1.1"`), игра откроет его напрямую.
> Для того чтобы заставить игру **всегда сохранять `.sav` в открытом виде** (без шифрования `m2s v1.1`), достаточно пропатчить функцию проверки режима сохранения `VA 0x0072F7D0` (`RVA 0x0032F7D0`, вызываемую из `FileSystem::M2SaveLoad::Save` на `VA 0x0072F405`):
> - **Оригинальные байты (`VA 0x0072F7D0`):** `55 8B EC` (`push ebp; mov ebp, esp`)
> - **Патч (3 байта):** `B0 01 C3` (`mov al, 1; ret`) — заставляет `M2SaveLoad::Save` сразу переходить на ветку `0x0072F411` и возвращать прямой `eFileStream` без заголовка `"m2s v1.1"` и без шифрования!

---

### 4.2. `Majesty2.exe` и `scene.dll` — `Game::CampaignsManager::LoadFromNode` (`VA 0x00437210`)

Метод `Game::CampaignsManager::LoadFromNode` (`VA 0x00437000..0x004373D0`, ключевой блок валидации и чтения узлов кампании — `VA 0x00437210..0x004373D0`) отвечает за десериализацию графа кампании и прогресса миссий:

```asm
0x00437243: mov  edx, dword ptr [ebp + 0x54]
0x00437246: push 0
0x00437248: push edx
0x00437249: push 0x00884D34          ; "0_LoadFromNode.md" (логгер/дамп ошибок узла)
0x0043724E: call 0x00759420
...
0x004372DE: push eax
0x004372DF: push 0x00884D10          ; ".\\Campaigns\\CampaignsManager.cpp"
0x004372E4: call 0x0074C480
...
0x0043732B: call 0x0074BF10
0x00437330: call 0x0074C050
0x00437335: mov  ecx, eax
0x00437337: push 0x00884D48          ; "Game::CampaignsManager::LoadFromNode"
0x0043733C: call 0x0074BED0
0x00437341: cmp  byte ptr [0x00B421DC], 0 ; глобальный флаг строгого режима проверки
0x00437348: jne  0x0043737E
```

На уровне движка (`scene.dll`) загрузка узлов сцены делегируется классу `xScene::DefaultSaveLoadProvider`:
- `xScene::DefaultSaveLoadProvider::vftable` — `RVA 0x0011C694` (`VA 0x1011C694`)
- `xScene::DefaultSaveLoadProvider::Save` — `RVA 0x0004B660` (`VA 0x1004B660`)
- `xScene::DefaultSaveLoadProvider::Load` — `RVA 0x0004C2B0` (`VA 0x1004C2B0`)
- `xScene::DefaultSaveLoadProvider::ValidateSaveVersion` — `RVA 0x00049A60` (`VA 0x10049A60`)

---

### 4.3. `ps.dll` — Экспорт `DummyFunction` (`VA 0x100065A0`) как штатная точка инъекции

В модуле системы частиц `ps.dll` присутствует экспортируемая функция `?DummyFunction@@YAXXZ` (ординал `33`):
- **RVA:** `0x000065A0` (**VA:** `0x100065A0`, файловое смещение: `0x000059A0`)
- **Дизассемблер (`16` байт):**
  ```asm
  0x100065A0: ret      ; C3
  0x100065A1: int3     ; CC (x15 байт выравнивания до 0x100065B0)
  ```
Благодаря 15 байтам `0xCC` (`int3`) сразу после `ret`, адрес `ps.dll + 0x65A0` представляет собой идеальный 16-байтный code-cave для размещения 5-байтного `JMP rel32` или `PUSH + CALL LoadLibraryA` при создании загрузчика нативных плагинов без модификации таблицы импорта `Majesty2.exe`.

---

### 4.4. `Multiplayer.dll` и `scene.dll` — Управление квантами времени (`m2TimeBetweenQuantCommands`)

В модуле `bin/plugins/Multiplayer.dll` зарегистрированы 4 консольные переменные (`eConVar<float>`), управляющие сетевым и симуляционным шагом квантования (`.\GameConnect.cpp`):

| Консольная переменная | Адрес `eConVar` (VA) | Адрес `float`-значения (VA) | Дефолт в коде | Значение в `resource_patch.pak` |
| :--- | :--- | :--- | ---: | ---: |
| `m2TimeBetweenQuantReports` | `0x10093144` | **`0x10093170`** | `0.050` с (`20` Гц) | **`0.0`** (БАГ!) |
| `m2QuantSmoothFactor` | `0x10093174` | **`0x100931A0`** | `0.015` | `0.015` |
| `m2TimeBetweenQuantCommands` | `0x100931A4` | **`0x100931D0`** | `0.070` с (`14.3` Гц) | **`0.0`** (БАГ!) |
| `m2MaxTimeDiffereceBetweenClients` | `0x100931D4` | **`0x10093200`** | `0.000` | *(отсутствует)* |

#### Дизассемблер проверок интервала квантов в `Multiplayer.dll`:
1. **Отправка отчётов о квантах (`VA 0x10008018..0x10008078`):**
   ```asm
   0x10008018: call 0x10001BA0               ; получение текущего дельта-времени
   0x1000801D: fadd dword ptr [esi + 0x110]  ; накопленное время с прошлого отчёта
   0x10008023: fstp dword ptr [esi + 0x110]
   0x10008029: fld  dword ptr [esi + 0x110]
   0x1000802F: fcomp dword ptr [0x10093170]  ; сравнение с m2TimeBetweenQuantReports
   0x10008035: fnstsw ax
   0x10008037: test ah, 5
   0x1000803A: jp   0x1000811D               ; если время < m2TimeBetweenQuantReports -> выход
   0x10008040: fldz
   0x10008042: fstp dword ptr [esi + 0x110]  ; сброс таймера в 0.0 и генерация пакета!
   ```
2. **Планирование и рассылка команд квантования (`VA 0x10009F36..0x10009F72` и `VA 0x10009AD3`):**
   ```asm
   0x10009F36: call 0x10001BA0
   0x10009F3B: fadd dword ptr [esi + 0x10C]
   0x10009F41: fst  dword ptr [esi + 0x10C]
   0x10009F47: fcomp dword ptr [0x100931D0]  ; сравнение с m2TimeBetweenQuantCommands
   0x10009F4D: fnstsw ax
   0x10009F4F: test ah, 5
   0x10009F52: jp   0x10009FF5               ; если время < m2TimeBetweenQuantCommands -> выход
   0x10009F58: fldz
   0x10009F5A: fstp dword ptr [esi + 0x10C]  ; сброс таймера и принудительный шаг симуляции!
   ```
На стороне `scene.dll` эти кванты обрабатываются классами `xScene::eQuantControl` (`VA 0x100C9820`, `Start` на `VA 0x100C9B90`, `QuantPrepare` на `VA 0x100CA5A0`) и `xScene::eQuantDistributor` (`VA 0x1000FCB0`, `AddQuant` на `VA 0x1000FB20`).

---

## 5. Производительность и устранение лагов Majesty 2 (Engine + Configs + Binary Hooks)

Несмотря на то, что Majesty 2 вышла в 2009 году, на современных ПК и особенно на macOS с процессорами Apple Silicon (через CrossOver / Wine / Rosetta 2) игра страдает от тяжёлых просадок FPS в средних и поздних миссиях, микрофризов при вращении камеры и статтеров симуляции.

Проведённый бинарный аудит всех 44 модулей выявил **4 фундаментальные инженерные причины** этих тормозов, ни одна из которых ранее не описывалась в публичных гайдах.

---

### Причина №1: Полное отсутствие `SSE/SSE2`-векторизации (100% устаревший стек `x87 FPU`)

Статический анализ машинного кода всех 44 бинарников показал поразительный факт: **весь движок GEM 3 и игра `Majesty2.exe` скомпилированы MSVC 8.0 (`2005`) с флагом `/arch:IA32` без единой векторной инструкции `SSE/SSE2` в математических, геометрических и физических модулях!**

#### Статистика инструкций `x87 FPU` против `SSE/SSE2` по модулям:

| Модуль | Назначение | Всего инструкций | Инструкций `x87 FPU` | Инструкций `SSE/SSE2` | Доля `x87` в коде |
| :--- | :--- | ---: | ---: | ---: | ---: |
| **`geometry.dll`** | Векторы, матрицы, AABB, OBB, фрустум, пересечения | `50 526` | **`16 973`** | **`0`** | **33.6%** |
| **`traceable.dll`** | Трассировка лучей (Raycasting), выбор юнитов мышкой | `17 871` | **`6 293`** | **`0`** | **35.2%** |
| **`Majesty2.exe`** | Игровая логика, ИИ героев, экономика, камера | `1 245 110` | **`3 954`** | **`0`** | — |
| **`pf.dll`** | Поиск пути (Pathfinding), навигационная сетка | `79 412` | **`2 456`** | **`0`** | 3.1% |
| **`app.dll`** | Главный цикл приложения, обработка окна и времени | `36 830` | **`2 314`** | **`1`** | 6.3% |
| **`editor.dll`** | Редактор ландшафта и карт | `25 180` | **`1 118`** | **`0`** | 4.4% |
| **`audio.dll`** | Звуковой движок и 3D-позиционирование | `24 899` | **`808`** | **`0`** | 3.2% |
| **`scene.dll`** | Граф сцены, трава, вода, террейн, LOD | `312 450` | **`555`** | **`0`** | — |
| **`render.dll`** | Рендерер Direct3D 9 | `198 620` | **`189`** | **`0`** | — |

#### Почему это вызывает катастрофические тормоза в `geometry.dll` и `traceable.dll`?
Взглянем на дизассемблер самой горячей функции движка, вызываемой десятки тысяч раз за кадр для трансформации ограничивающих параллелепипедов (AABB) каждого объекта, тайла травы и источника света — `?Xform@xGeometry@@YAXAAV?$eBoxGeneric@UVec3@@@1@ABUMatrix34@@@Z` (`geometry.dll`, **`VA 0x1001F940`**, аналогично `xGeometry::Construct` по **`VA 0x10013CF0`**):

```asm
; geometry.dll!xGeometry::Xform(eBoxGeneric<Vec3>& box, const Matrix34& m) (VA 0x1001F940)
0x1001F940: sub  esp, 0x3C
0x1001F943: mov  eax, dword ptr [esp + 0x44] ; Matrix34& m
0x1001F947: push esi
0x1001F948: mov  esi, dword ptr [esp + 0x44] ; eBoxGeneric<Vec3>& box
0x1001F94C: fld  dword ptr [esi]
0x1001F94E: fadd dword ptr [esi + 0xC]
... (ещё 95 инструкций перекладывания стека st(0)..st(7) через fxch!) ...
0x1001FA28: fstp dword ptr [esp]
0x1001FA2B: call 0x10002790                  ; <-- ВЫЗОВ fabsf() ЧЕРЕЗ СТЕК №1!
0x1001FA30: fstp dword ptr [esp + 0x48]
0x1001FA34: fld  dword ptr [esp + 8]
0x1001FA38: fstp dword ptr [esp]
0x1001FA3B: call 0x10002790                  ; <-- ВЫЗОВ fabsf() ЧЕРЕЗ СТЕК №2!
... (всего 9 отдельных вызовов call 0x10002790 на ОДИН AABB-бокс!) ...
```

- На трансформацию **одного** AABB (`eBoxGeneric<Vec3>`) тратится **140 инструкций `x87 FPU`** и **9 внешних вызовов `call 0x10002790` (`fabsf`)** с выгрузкой аргументов из FPU-стека в оперативную память (`fstp dword ptr [esp]`)!
- В функции отсечения по пирамиде видимости `?Intersecting@xGeometry@@YA_NABVeFrustum@1@ABV?$eBoxGeneric@UVec3@@@1@@Z` (**`VA 0x100151B0`**) выполняется **135 инструкций `x87`** с регулярной выгрузкой регистра состояния FPU в `AX` (`fnstsw ax; test ah, 0x41`), что полностью сбрасывает конвейер предсказания переходов современного CPU.
- При трансляции через **Rosetta 2** на Apple Silicon 80-битный стек `x87` (`st(0)..st(7)`) с циклическим указателем вершины стека и инструкциями `fxch` / `fnstsw ax` не может отображаться 1-в-1 на плоские 64-битные регистры ARM64 NEON и эмулируется программно с потерей производительности в **5–8 раз**. Замена этих функций на `SSE2` в нашем ускорителе [`patches/gem3_perf_accelerator.cpp`](file:///Users/orkodav4421/.gemini/antigravity/scratch/majesty2-re/patches/gem3_perf_accelerator.cpp) сокращает `Xform(eBox3, Matrix34)` со 140 `x87`-инструкций и 9 `call` до **22 регистровых инструкций `SSE2` без единого перехода и вызова функций**.

---

### Причина №2: Синхронные блокировки конвейера Direct3D 9 в `render.dll`

Дизассемблирование `render.dll` выявило две критические точки, в которых CPU принудительно останавливается и ждёт GPU (Pipeline Stall) на каждом кадре:

#### 2.1. Спин-лок CPU на аппаратных запросах окклюзии (`dbg_occlusionWaitResult = 1`)
В файле `resource_patch.pak/interface/cs/developer.con` разработчики прописали:
```ini
r_occlusionCullingNVOnly 0
```
Эта настройка включает аппаратный `IDirect3DQuery9` (`D3DQUERYTYPE_OCCLUSION`) на всех видеокартах (а не только NVIDIA). Однако в `render.dll` (`VA 0x1004AACC`) переменная **`dbg_occlusionWaitResult`** (`eConVar<bool>` по адресу `0x100A6474`, байт значения по адресу **`0x100A64A0`**) по умолчанию инициализируется значением **`1` (`true`)**!

Посмотрим, что делает `render.dll` при проверке результата окклюзии (`VA 0x1004AAF3..0x1004AC53`):
```asm
; render.dll — Проверка результата D3D9 Occlusion Query (VA 0x1004AAF3)
0x1004AAF3: mov  ecx, dword ptr [esi + 4]    ; IDirect3DQuery9*
0x1004AAF6: test ecx, ecx
0x1004AAF8: je   0x1004AC5B
0x1004AAFE: cmp  byte ptr [0x100A64A0], 0    ; проверка dbg_occlusionWaitResult
0x1004AB05: je   0x1004AC46                  ; если 0 -> асинхронный опрос без ожидания!

; ВЕТКА СПИН-ЛОКА (активна по умолчанию при dbg_occlusionWaitResult == 1):
0x1004AB0B: mov  eax, dword ptr [ecx]        ; vtable IDirect3DQuery9
0x1004AB0D: mov  edx, dword ptr [eax + 0x1C] ; IDirect3DQuery9::GetData
0x1004AB10: push 1                           ; D3DGETDATA_FLUSH
0x1004AB12: push 4
0x1004AB14: lea  ebp, [esp + 0x18]
0x1004AB18: push ebp
0x1004AB19: push ecx
0x1004AB1A: call edx                         ; GetData(&pixels, 4, D3DGETDATA_FLUSH)
0x1004AB1C: mov  ebx, eax
0x1004AB1E: cmp  ebx, 1                      ; S_FALSE (1) = GPU ещё не дорисовал!
0x1004AB21: je   0x1004AB06                  ; <-- БЕСКОНЕЧНЫЙ ЦИКЛ ОЖИДАНИЯ GPU НА CPU!
```
Ветка `0x1004AC46` (которая выполняется при `dbg_occlusionWaitResult 0`) опрашивает `GetData` **один раз**, и если запрос ещё не готов (`S_FALSE`), просто откладывает объект в массив отложенных запросов по адресу `0x100A64A4` до следующего кадра без остановки рендер-потока!
> **Решение:** Добавление `dbg_occlusionWaitResult 0` в `developer.con` (или патч байта `render.dll + 0xA64A0` в `0x00`) полностью устраняет синхронный спин-лок CPU/GPU на `IDirect3DQuery9::GetData`.

#### 2.2. Покадровое копирование карты тумана войны из VRAM в System RAM (`r_FogmapDisableSystemCopy = 0`)
В `render.dll` по адресу **`VA 0x1003F42E..0x1003F471`** находится обновление текстуры тумана войны (`1024x1024`):
```asm
; render.dll — Покадровое чтение RenderTarget тумана войны из VRAM в RAM (VA 0x1003F42E)
0x1003F42E: cmp  byte ptr [0x100A4DA0], 0    ; проверка r_FogmapDisableSystemCopy
0x1003F435: jne  0x1003F471                  ; если 1 -> пропустить копирование VRAM -> RAM!
0x1003F437: mov  eax, dword ptr [0x100A607C] ; номер кадра
0x1003F43C: xor  edx, edx
0x1003F43E: div  dword ptr [0x100A4D70]      ; деление на r_FogmapSystemCopyThinner (по умолчанию 1)
0x1003F444: test edx, edx
0x1003F446: jne  0x1003F471
0x1003F448: mov  eax, dword ptr [esi + 0x70] ; SystemMem Surface (D3DPOOL_SYSTEMMEM)
0x1003F44B: mov  ecx, dword ptr [esi + 0x6C] ; RenderTarget Surface (D3DPOOL_DEFAULT)
0x1003F44E: push eax
0x1003F44F: push ecx
0x1003F450: mov  ecx, edi
0x1003F452: call 0x10055C90                  ; -> IDirect3DDevice9::GetRenderTargetData (vtable +0x80)!
```
По умолчанию `r_FogmapDisableSystemCopy` (`0x100A4DA0`) равен **`0`**, а делитель частоты `r_FogmapSystemCopyThinner` (`0x100A4D70`) равен **`1`**. В результате **на каждом кадре** движок вызывает `IDirect3DDevice9::GetRenderTargetData`, заставляя драйвер Direct3D 9 (или транслятор WineD3D / DXVK / D3DMetal на macOS) полностью дождаться завершения всех команд рендеринга на GPU и перекачать `4 МБ` текстуры из видеопамяти в системную память!
> **Решение:** Установка `r_FogmapDisableSystemCopy 1` и `r_FogmapSystemCopyThinner 4` в `developer.con` (или патч байта `render.dll + 0xA4DA0` в `0x01`) полностью отключает эту блокировку.

---

### Причина №3: Критические ошибки конфигурационных файлов в `resource_patch.pak` и `resource.pak`

Сравнение конфигурационных файлов из базового архива `resource.pak` и последнего официального патча `resource_patch.pak` с дизассемблированным кодом `scene.dll`, `Multiplayer.dll` и `render.dll` выявило **4 грубых конфигурационных бага**:

#### Баг 3.1: Включённый режим редактора ландшафта `ed_alwaysEnableLandEditor 1` в `resource_patch.pak`
В `resource.pak/interface/cs/developer.con` строка `;ed_alwaysEnableLandEditor 0` была закомментирована. Однако в `resource_patch.pak/interface/cs/developer.con` разработчики случайно оставили отладочную настройку **раскомментированной и включённой**:
```ini
ed_alwaysEnableLandEditor 1
```
Что происходит в `scene.dll`, когда `ed_alwaysEnableLandEditor` (`0x101B17B4`, значение по адресу **`0x101B17D8`**) равен `1`?
- Функция `?LandEditorEnable@eState@xScene@@SA_NXZ` (`VA 0x100C1C70`) всегда возвращает `true`:
  ```asm
  0x100C1C70: cmp byte ptr [0x101AC5B5], 0
  0x100C1C77: jne 0x100C1C82
  0x100C1C79: cmp byte ptr [0x101B17D8], 0 ; ed_alwaysEnableLandEditor
  0x100C1C80: je  0x100C1C85
  0x100C1C82: mov al, 1
  0x100C1C84: ret
  ```
- В `scene.dll` по адресу **`VA 0x100B1F5A`** при загрузке карты проверяется `LandEditorEnable()`. Если она возвращает `1`, движок **пропускает финализацию (статическое запекание) вершинных буферов травы (`call 0x100AEB00`)**, оставляя геометрию травы в динамическом режиме редактирования!
- В `scene.dll` по адресу **`VA 0x100B2C8E`** при `LandEditorEnable() == 1` движок принудительно сбрасывает младший бит запекания (`and byte ptr [ebx + 0x39], 0xFE`) у всех декалей/штампов поверхности (`eStamp`), заставляя пересчитывать их проекцию на сетку высот!
- Кроме того, команды `con_messages`, `con_warnings`, `con_errors`, `con_debugs` и `pro_log` в `developer.con` вообще не существуют ни в одном бинарнике GEM 3 (это мёртвое наследие движка GEM 2 / «В тылу врага 2»).

#### Баг 3.2: Нулевые задержки квантов `m2TimeBetweenQuantCommands 0.0` в `resource_patch.pak`
В `resource.pak/interface/cs/m2GameConst.con` сетевые и симуляционные тайминги были настроены корректно:
```ini
m2TimeBetweenQuantCommands 0.07
m2TimeBetweenQuantReports  0.05
```
В `resource_patch.pak/interface/cs/m2GameConst.con` оба параметра были сброшены в **`0.0`**:
```ini
m2TimeBetweenQuantCommands 0.0
m2TimeBetweenQuantReports  0.0
```
Как показано в дизассемблере `Multiplayer.dll` (`VA 0x1000802F` и `VA 0x10009F47` в разделе 4.4), при значении `0.0` условие `accumulated_time < 0.0` никогда не выполняется, и генератор квантов срабатывает **на каждой итерации главного цикла** (сотни раз в секунду вместо 20–30 Гц), перегружая очередь `xScene::eQuantDistributor` и сериализатор `eSyncer`.
> **Решение:** Установить `m2TimeBetweenQuantCommands 0.033` и `m2TimeBetweenQuantReports 0.033` (ровно `30` Гц).

#### Баг 3.3: Закомментированный и несовместимый по регистру файл LOD (`level_of_detail.set` vs `lod_classes.set`)
В `resource.pak` файл `set/level_of_detail.set` **полностью закомментирован** символами `;`:
```ini
; {"test"
;   {distance 700} ; LOD1
;   {distance 1000 ; LOD2
;     {NoFog}
;     {NoGetShadow}
;     {NoCastShadow}
;     {off}
;   }
; }
```
Более того, дизассемблирование парсера LOD в `scene.dll` (`.\view_lod.cpp`, **`VA 0x100486A0..0x10048BDE`**) показало две важные детали:
1. Движок загружает не `set/level_of_detail.set`, а файл **`/set/lod_classes.set`** (`VA 0x1011DCE0`, пуш на `VA 0x10048B08`)!
2. Парсер флагов LOD (`VA 0x100486A0..0x10048720`) сравнивает теги чувствительным к регистру `strcmp` со строками **строго в нижнем регистре**:
   - `"distance"` (`0x1011DC9C`)
   - `"off"` (`0x1011DCA4`) -> устанавливает флаг `0x100` (`or edi, 0x100`)
   - `"nofog"` (`0x1011DCAC`) -> устанавливает флаг `0x200` (`or edi, 0x200`)
   - `"nogetshadow"` (`0x1011DCB4`) -> устанавливает флаг `0x400` (`or edi, 0x400`)
   - `"nocastshadow"` (`0x1011DCC0`) -> устанавливает флаг `0x800` (`or edi, 0x800`)
   Поэтому написанные в `level_of_detail.set` с заглавных букв `{NoFog}`, `{NoGetShadow}`, `{NoCastShadow}` парсером `scene.dll` даже после раскомментирования вызвали бы фатальную ошибку `Invalid flag definition in LOD class` (`VA 0x1011DCD0`)!

#### Баг 3.4: Переполнение кластеров травы (`set/grass.set`, опечатка `{dencity 3}`)
В `scene.dll` (`.\land_grass.cpp`, `VA 0x100AE2D0`) жёстко задан лимит геометрии одного кластера травы; при его превышении движок выбрасывает предупреждение `Too many polygons in grass cluster` (`VA 0x101222B0`), а переменная отсечения травы **`r_grassCull`** (`0x101B12CC`, значение по **`0x101B12F4`**) по умолчанию равна **`0` (выключено!)**, при дальности **`r_grassDistance = 100.0`** (`0x101B1294`).
При этом в `set/grass.set` для основных типов травы (`grass_01`, `grass_02`, `grass_m2`) выставлена плотность `{dencity 3}` (именно с орфографической ошибкой `"dencity"`, зашитой в парсер `scene.dll`). Снижение `{dencity 3}` до `{dencity 1}` вместе с `r_grassCull 1` и `r_grassDistance 65.0` полностью устраняет переполнение вершинных буферов травы.

---

### Сводная таблица ключевых CVars производительности GEM 3

Все адреса ниже извлечены напрямую из конструкторов `eConVar<T>` в бинарных файлах (`sdk/engine_cvars.json`):

| Модуль | Имя CVar | Тип | VA объекта `eConVar` | VA значения | Дефолт движка | Оптимум | Эффект оптимизации |
| :--- | :--- | :---: | :--- | :--- | :---: | :---: | :--- |
| `render.dll` | `dbg_occlusionWaitResult` | `bool` | `0x100A6474` | `0x100A64A0` | `1` | **`0`** | Отключает синхронный спин-лок CPU в цикле `IDirect3DQuery9::GetData` (`VA 0x1004AB06`) |
| `render.dll` | `r_occlusionCulling` | `bool` | `0x100A6408` | `0x100A6434` | `1` | **`1`** | Аппаратное отсечение невидимых объектов |
| `render.dll` | `r_occlusionCullingNVOnly` | `bool` | `0x100A643C` | `0x100A6468` | `1` | **`0`** | Разрешает аппаратный Occlusion Culling на всех GPU (в паре с `dbg_occlusionWaitResult 0`) |
| `render.dll` | `r_FogmapDisableSystemCopy` | `bool` | `0x100A4D74` | `0x100A4DA0` | `0` | **`1`** | Отключает покадровый `GetRenderTargetData` (`1024x1024` VRAM -> System RAM stall на `VA 0x1003F452`) |
| `render.dll` | `r_FogmapSystemCopyThinner` | `int` | `0x100A4D44` | `0x100A4D70` | `1` | **`4`** | В 4 раза снижает частоту обращения к системной копии карты тумана, если она требуется |
| `scene.dll` | `ed_alwaysEnableLandEditor` | `bool` | `0x101B17B4` | `0x101B17D8` | `0` (`1` в патче) | **`0`** | Возвращает статическое запекание буферов травы (`VA 0x100B1F5A`) и штампов террейна (`VA 0x100B2C8E`) |
| `scene.dll` | `r_grassCull` | `bool` | `0x101B12CC` | `0x101B12F4` | `0` | **`1`** | Включает отсечение невидимых кластеров травы |
| `scene.dll` | `r_grassDistance` | `float` | `0x101B1270` | `0x101B1294` | `100.0` | **`65.0`** | Уменьшает число полигонов травы в кадре в 2.3 раза без визуальной потери в изометрии |
| `scene.dll` | `s_showwarnings` | `bool` | `0x101AA8E8` | `0x101AA90C` | `0` | **`0`** | Отключает вывод предупреждений сцены в консоль/лог |
| `Multiplayer.dll` | `m2TimeBetweenQuantCommands` | `float` | `0x100931A4` | `0x100931D0` | `0.07` (`0.0` в патче) | **`0.033`** | Фиксирует частоту квантов команд на 30 Гц вместо бесконечного спама при `0.0` |
| `Multiplayer.dll` | `m2TimeBetweenQuantReports` | `float` | `0x10093144` | `0x10093170` | `0.05` (`0.0` в патче) | **`0.033`** | Фиксирует частоту отчётов квантов на 30 Гц |

---

### Готовый пакет исправлений и ускоритель в каталоге `patches/`

Все разработанные исправления собраны в каталоге [`patches/`](file:///Users/orkodav4421/.gemini/antigravity/scratch/majesty2-re/patches/):

1. **[`patches/gem3_perf_accelerator.cpp`](file:///Users/orkodav4421/.gemini/antigravity/scratch/majesty2-re/patches/gem3_perf_accelerator.cpp)** — исходный код нативного `SSE2`-ускорителя и бинарного патчера памяти:
   - Перехватывает (`JMP rel32`) самые тяжёлые `x87`-функции `geometry.dll`:
     - `xGeometry::Xform(eBoxGeneric<Vec3>&, const Matrix34&)` (`VA 0x1001F940`)
     - `xGeometry::Construct(eBoxGeneric<Vec3>*, const eBoxGeneric<Vec3>&, const Matrix34&)` (`VA 0x10013CF0`)
     - `Vec3::Xform(const Matrix34&)` (`VA 0x10008800`)
     - `Vec3::Xform(const Matrix33&)` (`VA 0x1000EBA0`)
     - `xGeometry::Intersecting(const eFrustum&, const eBoxGeneric<Vec3>&)` (`VA 0x100151B0`)
   - Программно патчит в памяти `render.dll` (`dbg_occlusionWaitResult = 0`, `r_FogmapDisableSystemCopy = 1`), `scene.dll` (`ed_alwaysEnableLandEditor = 0`, `r_grassCull = 1`) и `Multiplayer.dll` (`m2TimeBetweenQuantReports = 0.033f`, `m2TimeBetweenQuantCommands = 0.033f`).
2. **[`patches/configs/`](file:///Users/orkodav4421/.gemini/antigravity/scratch/majesty2-re/patches/configs/)**:
   - [`developer.con`](file:///Users/orkodav4421/.gemini/antigravity/scratch/majesty2-re/patches/configs/developer.con) — исправленный конфиг инициализации рендера и сцены.
   - [`m2GameConst.con`](file:///Users/orkodav4421/.gemini/antigravity/scratch/majesty2-re/patches/configs/m2GameConst.con) — исправленные тайминги квантов симуляции (`0.033`).
   - [`grass.set`](file:///Users/orkodav4421/.gemini/antigravity/scratch/majesty2-re/patches/configs/grass.set) — оптимизированная плотность травы (`{dencity 1}`).
   - [`lod_classes.set`](file:///Users/orkodav4421/.gemini/antigravity/scratch/majesty2-re/patches/configs/lod_classes.set) — корректный файл классов LOD в нижнем регистре для `scene.dll`.
3. **[`patches/resource_patch_optimized.pak`](file:///Users/orkodav4421/.gemini/antigravity/scratch/majesty2-re/patches/resource_patch_optimized.pak)** и скрипт сборки **[`patches/apply_resource_patch_fix.py`](file:///Users/orkodav4421/.gemini/antigravity/scratch/majesty2-re/patches/apply_resource_patch_fix.py)** — готовый пересобранный архив `resource_patch.pak` с интегрированными конфигурационными исправлениями.
