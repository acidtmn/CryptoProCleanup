# КриптоПро Очистка 0.5.3 RC1

> Публичный кандидат для проверки перед стабильным выпуском. Проект не связан с ООО «КРИПТО-ПРО» и не одобрен правообладателем продуктов CryptoPro.

Portable-утилита помогает сохранить полные номера лицензий и открытые сертификаты, корректно запустить зарегистрированные деинсталляторы CryptoPro, убрать только заранее подтверждённые остатки и извлечь данные из Windows на подключённом диске.

![Обзор системы](https://raw.githubusercontent.com/acidtmn/CryptoProCleanup/v0.5.3-rc1/docs/images/0.5.3/overview.png)

## Что скачать

| Файл | Назначение |
|---|---|
| `CryptoProCleanup-0.5.3-rc1-windows.zip` | Полный комплект: Modern, Legacy, документация, исходники и внутренний `SHA256SUMS.txt` |
| `CryptoProCleanup-Modern-x64-0.5.3-rc1.zip` | Рекомендуется для Windows 10/11 x64; распакуйте весь архив и не отделяйте EXE от соседних файлов |
| `CryptoProCleanup-Legacy-x86-0.5.3-rc1.exe` | Один EXE для Windows 7 SP1, 8, 8.1, 10 и 11 на x86/x64 |
| `CryptoProCleanup-0.5.3-rc1-source.zip` | Снимок открытого исходного кода релиза |
| `SHA256SUMS-0.5.3-rc1.txt` | SHA-256 всех перечисленных релизных файлов |

Для Windows 10/11 x64 выбирайте Modern. Для Windows 7/8/8.1 и любой 32-битной Windows — Legacy. Обе редакции используют одно и то же ядро обнаружения, резервного копирования и безопасной очистки.

## Главное в версии 0.5.3 RC1

- современный адаптивный Modern-интерфейс на C++/WinUI 3 с тёмной, светлой и системной темами;
- компактная Legacy-раскладка, в которой основные действия остаются доступны при 800×600 и 1024×768;
- безопасное сканирование без изменений сразу после запуска;
- извлечение, просмотр и копирование полного номера лицензии CryptoPro;
- выбор и экспорт только открытой части пользовательских сертификатов в `.cer` и общий `.p7b`;
- штатная MSI/EXE-деинсталляция первой, остаточная очистка — только после неё и только по проверенному плану;
- поддержка обнаруживаемых установок CSP 3.x, 4.x и 5.x без жёсткой привязки к одному ProductCode;
- отдельная страница «Офлайн-Windows»: можно выбрать корень диска или папку `Windows`, извлечь лицензии и открытые сертификаты и получить понятную индикацию долгого сканирования;
- отчёты без полных лицензий и персональных имён сертификатов; конфиденциальные данные сохраняются отдельно;
- защищённое продолжение после перезагрузки без самостоятельной перезагрузки компьютера;
- RU/EN, масштабируемое окно, DPI-aware интерфейс, фирменная иконка, ссылки на GitHub, сайт автора и поддержку проекта.

## Что программа намеренно сохраняет

- закрытые ключи и контейнеры ключей;
- аппаратные токены и смарт-карты;
- системные хранилища сертификатов;
- неизвестные файлы и ветки реестра, принадлежность которых CryptoPro не подтверждена;
- системный кэш Windows Installer.

Перед удалением выберите папку резервной копии вне системного диска. Если программа не может записать резервную копию, очистка не начинается.

## Важные ограничения кандидата

- сборки пока не подписаны цифровой подписью, поэтому SmartScreen может показать предупреждение;
- полная VM-матрица Windows 7 SP1–Windows 11 и всех поколений CSP ещё не завершена;
- автоматического тихого удаления нет;
- принудительная очистка доступна только после ошибки штатного деинсталлятора, отдельного подтверждения и в пределах уже проверенного плана;
- открытые `.cer`/`.p7b` не содержат закрытый ключ и сами по себе не позволяют подписывать документы.

Локально пройдены Release-сборки и native unit tests для Win32/x64, проверки ресурсов тем и отступов, Modern UI smoke для шести страниц и RU/EN, а также Legacy UI smoke на малых разрешениях. Полная матрица и разрушительные сценарии остаются обязательными перед стабильным релизом: [подробный журнал проверки](https://github.com/acidtmn/CryptoProCleanup/blob/v0.5.3-rc1/docs/TEST_MATRIX.md).

<details>
<summary><strong>Скриншоты интерфейса</strong></summary>

### Открытые сертификаты

![Открытые сертификаты](https://raw.githubusercontent.com/acidtmn/CryptoProCleanup/v0.5.3-rc1/docs/images/0.5.3/certificates.png)

### Отключённая Windows

![Офлайн-Windows](https://raw.githubusercontent.com/acidtmn/CryptoProCleanup/v0.5.3-rc1/docs/images/0.5.3/offline-windows.png)

### Журнал и отчёты

![Журнал и отчёты](https://raw.githubusercontent.com/acidtmn/CryptoProCleanup/v0.5.3-rc1/docs/images/0.5.3/reports.png)

### Настройки

![Настройки](https://raw.githubusercontent.com/acidtmn/CryptoProCleanup/v0.5.3-rc1/docs/images/0.5.3/settings.png)

### О программе

![О программе](https://raw.githubusercontent.com/acidtmn/CryptoProCleanup/v0.5.3-rc1/docs/images/0.5.3/about.png)

</details>

## English

This is a public release candidate, not a stable release. Use the Modern x64 ZIP on Windows 10/11 x64 and the standalone Legacy x86 executable on Windows 7 SP1/8/8.1 or any 32-bit Windows installation. Both editions share the same safety core.

The utility backs up complete license identifiers and selected public certificates, invokes registered uninstallers before verified residual cleanup, and can read licenses and public certificates from a disconnected Windows disk. Private keys, tokens, certificate stores, unknown remnants, and the Windows Installer cache are deliberately preserved.

The binaries are currently unsigned and may trigger SmartScreen. Verify downloads with `SHA256SUMS-0.5.3-rc1.txt`. See the [English README](https://github.com/acidtmn/CryptoProCleanup/blob/v0.5.3-rc1/README.en.md) for full usage and safety details.

---

Автор: **Кирилл Александров** · [kodalexandrova.ru](https://kodalexandrova.ru) · [Поддержать проект](https://yoomoney.ru/to/4100119195083142) · лицензия [MIT](https://github.com/acidtmn/CryptoProCleanup/blob/v0.5.3-rc1/LICENSE)
