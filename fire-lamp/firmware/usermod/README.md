# AutoNightLight — актуальный исходный код

**S3-PWR-R2 использует VEML7700, не BH1750.** Этот каталог больше не содержит активную копию пользовательского модуля, чтобы исключить расхождение с основным репозиторием.

Поддерживаемая реализация находится в приватном [LumaCore-Labs/lumacore-firmware](https://github.com/LumaCore-Labs/lumacore-firmware):

- `targets/s3-pwr/wled/usermods/usermod_lumacore_pir_bh1750/` — модуль AutoNightLight;
- `lumacore_ambient_light.h` в этом каталоге — драйверы VEML7700 и исторического BH1750;
- `targets/s3-pwr/wled/include/lumacore_veml7700_math.h` — алгоритм автодиапазона.

Историческое имя `usermod_lumacore_pir_bh1750` используется для совместимости; тип датчика выбирается флагом сборки `LUMACORE_AMBIENT_SENSOR_VEML7700`. Все исходники для реальной сборки берите **целиком из основного репозитория**, не соединяя его файлы с архивной реализацией.

Старая автономная публикация перенесена [в архив](../legacy/bh1750/) без изменения исходников.
