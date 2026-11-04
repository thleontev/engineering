# Публичные примеры WLED

`cfg.example.json` — обезличенный **пример структуры WLED**, обновлённый в части настроек Fire Lamp / LumaCore-S3-PWR-R2. Для R2 здесь показаны: 256 LED, GPIO17 DATA, GPIO21 LED enable, GPIO1 touch, I²C GPIO47/48, программный лимит 6500 мА, `AutoNightLight.LuxThreshold = 0.1` лк, `ManualOffPauseSec = 600` с и включение после старта `off`.

`presets.example.json` — **исторический демонстрационный набор эффектов**, не актуальные заводские presets R2.

Эти примеры не являются точной копией рабочего состояния устройства и не предназначены для прямой загрузки на контроллер. Перед прошивкой необходимы актуальные профили, manifest, процедура и проверка совместимости из приватного [LumaCore-Labs/lumacore-firmware](https://github.com/LumaCore-Labs/lumacore-firmware) (`targets/s3-pwr/profiles/factory/s3-pwr-r2/`; `docs/PROVISIONING.md`).

Поля `<SET_DURING_PROVISIONING>` заполняются локально. Никогда не публикуйте реальные пароли и Device Identity. Программный лимит WLED не заменяет аппаратную защиту LED-питания.
