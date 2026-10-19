# КриптоПро Очистка 0.7.0

## Три задачи вместо запутанного экрана

Новая Modern-версия для **Windows 10 1809+ / Windows 11, x64**. На главной сразу выбираешь нужное: **сохранить данные**, **удалить программы** или **проверить другой диск**. Сохранение копии не запускает удаление.

**Предварительный релиз:** локально собран и проверен на Windows 11 x64. Полная матрица Windows/CSP и реальное удаление в этом выпуске ещё не проверялись.

![Главная Modern 0.7.0](https://raw.githubusercontent.com/acidtmn/CryptoProCleanup/v0.7.0/docs/screenshots/0.7.0/01-home.png)

## Скачать и запустить

| Что нужно | Файл |
|---|---|
| Программа для Windows 10/11 x64 | [CryptoProCleanup-Modern-x64-0.7.0.zip](https://github.com/acidtmn/CryptoProCleanup/releases/download/v0.7.0/CryptoProCleanup-Modern-x64-0.7.0.zip) |
| Программа, документация и исходники вместе | [CryptoProCleanup-0.7.0-windows.zip](https://github.com/acidtmn/CryptoProCleanup/releases/download/v0.7.0/CryptoProCleanup-0.7.0-windows.zip) |
| Только исходный код | [CryptoProCleanup-0.7.0-source.zip](https://github.com/acidtmn/CryptoProCleanup/releases/download/v0.7.0/CryptoProCleanup-0.7.0-source.zip) |
| Контрольные суммы трёх архивов | [SHA256SUMS-0.7.0.txt](https://github.com/acidtmn/CryptoProCleanup/releases/download/v0.7.0/SHA256SUMS-0.7.0.txt) |

**Распакуй ZIP полностью и запусти `CryptoProCleanup.exe`.** Не вынимай EXE из папки: соседние файлы нужны для работы. Установка .NET, PowerShell и отдельного WinUI runtime не требуется. Запуск запрашивает права администратора, затем выполняет безопасное сканирование.

Для Windows 7/8/8.1 и 32-битной Windows остаётся [отдельная Legacy-версия 0.6.0-preview1](https://github.com/acidtmn/CryptoProCleanup/releases/download/v0.6.0-preview1/CryptoProCleanup-Legacy-x86-0.6.0-preview1.exe). Она в этот выпуск не входит и не обновлялась.

## Что стало удобнее

- **Понятная главная.** Три крупных действия, состояние системы и быстрый доступ к полному номеру лицензии.
- **Удаление отдельно.** Выбор продуктов и профилей, резервная копия, проверка плана, затем подтверждение. Сначала штатный MSI/EXE-деинсталлятор, потом подтверждённые остатки.
- **Сертификаты под рукой.** Поиск, фильтры и экспорт открытой части в CER/P7B. Кнопка экспорта закреплена внизу и не теряется при прокрутке.
- **Другой диск.** Выбор корня диска или папки Windows, чтение найденных данных и отдельное сохранение без запуска очистки.
- **Свежий вид.** Графитовые поверхности, бирюзовый акцент, светлая/тёмная/системная темы, RU/EN и подписанная навигация. WinUI 3 / XAML / C++17, без браузерной оболочки.
- **Исправления.** Переходы с главной, минимальный размер окна 900×650, общий нативный выбор папки и очистка инициалов при скрытии сертификата фильтром.

## Что сохраняется

Перед изменениями создаётся копия: `licenses.txt` с полными лицензиями, выбранные открытые сертификаты, текстовый итог `summary.txt`, `report.json` и обезличенный `cleanup.log`. Если копия не записалась, удаление не начинается. Храни копию вне системного диска и не публикуй её: лицензии и сведения сертификатов конфиденциальны.

Собственная очистка не удаляет сертификатные хранилища, закрытые ключи, контейнеры или данные токенов. CER/P7B не содержат закрытых ключей и сами по себе не позволяют подписывать документы. Удаление криптографического ПО всё равно может нарушить подпись, VPN, вход или доступ к зашифрованным данным: сначала сохрани нужное и прочитай план.

<details>
<summary>Все разделы программы и светлая тема</summary>

### Удаление

![Подготовка удаления](https://raw.githubusercontent.com/acidtmn/CryptoProCleanup/v0.7.0/docs/screenshots/0.7.0/02-removal.png)

### Открытые сертификаты

Личные записи намеренно скрыты штатным поисковым фильтром. Это снимок настоящего интерфейса, а не демонстрационные сертификаты.

![Сертификаты](https://raw.githubusercontent.com/acidtmn/CryptoProCleanup/v0.7.0/docs/screenshots/0.7.0/03-certificates.png)

### Другой диск

![Офлайн-Windows](https://raw.githubusercontent.com/acidtmn/CryptoProCleanup/v0.7.0/docs/screenshots/0.7.0/04-offline.png)

### Отчёты

![Журнал и отчёты](https://raw.githubusercontent.com/acidtmn/CryptoProCleanup/v0.7.0/docs/screenshots/0.7.0/05-reports.png)

### Настройки

![Настройки](https://raw.githubusercontent.com/acidtmn/CryptoProCleanup/v0.7.0/docs/screenshots/0.7.0/06-settings.png)

### О программе

![О программе](https://raw.githubusercontent.com/acidtmn/CryptoProCleanup/v0.7.0/docs/screenshots/0.7.0/07-about.png)

### Светлая тема

![Светлая главная](https://raw.githubusercontent.com/acidtmn/CryptoProCleanup/v0.7.0/docs/screenshots/0.7.0/08-home-light.png)

</details>

## Проверки и ограничения

Локально: MSVC v143 Release x64, модульные тесты, ресурсы тем/раскладки, безопасное интеграционное копирование, семь разделов RU/EN, Dark/Light/System и окна 900×650, 1024×760, 1280×720. Проверено сохранение через интерфейс с одним выбранным открытым сертификатом, включая отмену и повторное открытие диалога. Финальная упакованная программа проверена отдельно.

Новая версия не запускалась отдельно на Windows 10 и ARM64. Реальное удаление продуктов, физический офлайн-диск, аппаратные токены, системный High Contrast и продолжение после перезагрузки в этом выпуске не проверялись. [Полная матрица проверок](https://github.com/acidtmn/CryptoProCleanup/blob/v0.7.0/docs/TEST_MATRIX.md) остаётся условием стабильного выпуска.

EXE **не подписан**: возможно предупреждение SmartScreen. SHA-256 проверяет целостность скачанного файла, но не заменяет цифровую подпись.

## English

Modern 0.7.0 is a **pre-release for Windows 10 1809+ / Windows 11 x64**. The new home offers three tasks: back up data, remove applications, or inspect another drive. Removal has its own workspace; certificate export remains visible in a fixed action bar. Light/Dark/System themes, RU/EN, native WinUI 3 / XAML / C++17, and the existing shared safety engine are retained.

Extract the entire Modern ZIP and run `CryptoProCleanup.exe`; keep all adjacent files. No separate .NET or WinUI runtime installation is needed. Public screenshots intentionally hide personal certificates with a search filter. Legacy for Windows 7/8 and x86 remains a separate 0.6.0-preview1 download.

Built and safely tested locally on Windows 11 x64. Full Windows/CSP, destructive removal, physical offline-drive, token and reboot tests were not performed for this release. The executable is unsigned. Backups may contain confidential licenses and certificate information. See the [English README](https://github.com/acidtmn/CryptoProCleanup/blob/v0.7.0/README.en.md).

---

Неофициальный открытый проект. Не связан с ООО «КРИПТО-ПРО» и не одобрен правообладателем.

Автор: **Кирилл Александров** · [Код Александрова](https://kodalexandrova.ru) · [Поддержать проект](https://yoomoney.ru/to/4100119195083142) · [Лицензия MIT](https://github.com/acidtmn/CryptoProCleanup/blob/v0.7.0/LICENSE)
