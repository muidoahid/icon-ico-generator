# icon-ico-generator

SVG veya PNG dosyasından çok boyutlu **ICO** üreten küçük bir Windows aracı (`icon.exe`).
Simgesiz, arayüzsüz, C++17. Komut satırından ya da dosyayı exe üzerine sürükleyip bırakarak kullanılır.

**Hazır exe:** [`bin/icon.exe`](bin/icon.exe) (Windows x64). Kaynak koddan derlenmiştir, ayrıca bir şey kurmak gerekmez.

## Kullanım

    icon -p "png dosyası" -o "ico çıktı klasörü"
    icon -s "svg dosyası" -o "ico çıktı klasörü"
    icon -p "png dosyası"          (ICO, girdinin klasörüne yazılır)
    icon -s "svg dosyası"

Sürükle-bırak: bir veya birden çok PNG / SVG dosyasını `icon.exe` üzerine bırakın; ICO'lar dosyaların
bulunduğu klasöre yazılır. Çift tıklayınca çalışmaz (bilgi penceresi gösterir).

Hatalar: cmd'den çalışırken cmd ekranına, sürükle-bırak ile çalışırken uyarı penceresine yazılır.
Çıkış kodu: 0 başarılı, 1 hata. Çıktı dosyası varsa üzerine yazılır.

## ICO içeriği

- 16, 24, 32, 48, 64, 128, 256 piksel, 32 bit RGBA (256 boyutu PNG sıkıştırmalı)
- 16–128 için ek olarak 8 bit paletli girişler (256 renk, 1 bit AND maskesi, Floyd-Steinberg)
- SVG her boyutta doğrudan o piksel boyutunda render edilir. PNG sRGB ve alfa ağırlıklı yeniden
  örnekleme ile küçültülür; büyütme yapılmaz (küçük PNG'de büyük boyutlar atlanır, uyarı verilir).
- Kare olmayan görseller şeffaf kare tuvale ortalanır.

## Sınırlar

- SVG motoru nanosvg'dir: metin, filtre, maske, clipPath, `use`, `image`, `pattern`, `style` desteklenmez
  (uyarı verilir). Metni yola çevirin.
- GUI alt sistemli olduğu için cmd'de istem hemen döner; çıktıdan sonra konsola bir Enter gönderilir ve istem
  yeniden çizilir. Betiklerde çıktıyı yönlendirirseniz (`icon ... > log.txt`) Enter gönderilmez, çıktı doğrudan
  dosyaya UTF-8 yazılır.

## Derleme

`build.bat` (zig gerekir) ya da elle:

    zig c++ -std=c++17 -O2 -s -Ithird_party src\icon.cpp -target x86_64-windows-gnu -Wl,--subsystem,windows -lshell32 -luser32 -o icon.exe

MSVC ile: `cl /std:c++17 /O2 /utf-8 /EHsc /I third_party src\icon.cpp /link /SUBSYSTEM:WINDOWS shell32.lib user32.lib`

## Test

`test.ps1` (PowerShell) örnek dosyalarla ICO boyutlarını, bit derinliklerini, hata durumlarını ve Windows'un
ICO okuyucusuyla açılabilirliği dener; sonuç `test.log` dosyasına yazılır.

## Üçüncü taraf

nanosvg (zlib), stb_image / stb_image_write / stb_image_resize2 (kamu malı / MIT). Lisansları `third_party/` içindedir.
