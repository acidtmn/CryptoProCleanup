# Security policy / Политика безопасности

## Confidential data

Never attach real `licenses.txt`, `certificates.txt`, certificate files, private-key containers, tokens, registry hives, or unredacted screenshots to a public issue. `report.json`, `summary.txt`, and `cleanup.log` are designed to omit full license values and certificate names, but review them before sharing.

Никогда не прикладывайте к публичным issue настоящие `licenses.txt`, `certificates.txt`, сертификаты, контейнеры закрытых ключей, токены, кусты реестра или нескрытые снимки экрана. Перед публикацией дополнительно проверьте даже обезличенные отчёты.

## Reporting a vulnerability

For a suspected vulnerability, do not open a public issue containing reproduction data. Contact the maintainer through [kodalexandrova.ru](https://kodalexandrova.ru) with a minimal description first. Include the affected version, Windows version, and whether any destructive action actually occurred, but do not send secrets until a secure channel is agreed.

Для сообщения об уязвимости сначала свяжитесь с автором через [kodalexandrova.ru](https://kodalexandrova.ru), не публикуя чувствительные данные. Укажите версию программы и Windows, а также выполнялось ли удаление.

Only the latest release candidate is actively maintained. Security fixes may be published without compatibility guarantees for older prereleases.
