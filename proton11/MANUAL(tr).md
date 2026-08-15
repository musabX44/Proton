# Proton Dil Rehberi — Eğitim, Öğrenme, Pekiştirme

Bu döküman `README.md`'nin (mimari + değişiklik günlüğü odaklı) yanına, **dili
sıfırdan öğrenmek isteyen biri için** yazıldı. Sırasıyla: kurulum, dilin
temelleri, tip sistemi, kontrol akışı, fonksiyonlar, struct/enum, diziler ve
map'ler, hata yönetimi, bellek modeli (LAM), native built-in'ler, ve tüm
`stdlib/` modüllerinin komple API referansı yer alıyor. Her bölümde çalışan,
kopyala-yapıştır örnekler var; hepsi bu depodaki gerçek dosyalarla test
edildi.

---

## İçindekiler

1. [Kurulum ve ilk çalıştırma](#1-kurulum-ve-ilk-çalıştırma)
2. [Dilin temelleri](#2-dilin-temelleri)
3. [Tip sistemi](#3-tip-sistemi)
4. [Kontrol akışı](#4-kontrol-akışı)
5. [Fonksiyonlar](#5-fonksiyonlar)
6. [Struct ve enum](#6-struct-ve-enum)
7. [Diziler (Lists) ve Map'ler](#7-diziler-lists-ve-mapler)
8. [Hata değerleri ve `?` operatörü](#8-hata-değerleri-ve--operatörü)
9. [Bellek modeli (LAM) — ne zaman önemli?](#9-bellek-modeli-lam--ne-zaman-önemli)
10. [Native built-in'ler (dil çekirdeğine gömülü)](#10-native-builtinler-dil-çekirdeğine-gömülü)
11. [Modül sistemi (`use`)](#11-modül-sistemi-use)
12. [`stdlib/string` — komple API referansı](#12-stdlibstring--komple-api-referansı)
13. [`stdlib/collections` — komple API referansı](#13-stdlibcollections--komple-api-referansı)
14. [`stdlib/os` — komple API referansı](#14-stdlibos--komple-api-referansı)
15. [`stdlib/math` — komple API referansı](#15-stdlibmath--komple-api-referansı)
16. [`stdlib/random` — komple API referansı](#16-stdlibrandom--komple-api-referansı)
17. [`stdlib/ml` — komple API referansı](#17-stdlibml--komple-api-referansı)
18. [`net::` — ağ kütüphanesi](#18-net--ağ-kütüphanesi)
19. [Sık yapılan hatalar ve tuzaklar](#19-sık-yapılan-hatalar-ve-tuzaklar)
20. [Alıştırmalar](#20-alıştırmalar)
21. [Hızlı komut/opcode kartı](#21-hızlı-komutopcode-kartı)

---

## 1. Kurulum ve ilk çalıştırma

```sh
make                      # gcc ile derler, proton6/proton çalıştırılabilirini üretir
./proton examples/hello.prt
```

`examples/hello.prt`:

```proton
fn main() {
    io::out("Hello, World!");
}
```

Her Proton programı bir `main()` fonksiyonundan başlar (C/Rust/Go gibi).
Dosya uzantısı `.prt`. `io::out(...)` değişken sayıda argüman alır, hepsini
art arda yazdırıp sona bir yeni satır ekler — `println!`/`console.log` gibi
düşünebilirsin.

Kendi scriptini çalıştırmak için:

```sh
./proton path/to/script.prt [ek argümanlar...]
```

Ek argümanlara scriptin içinden `sys::args()` ya da `os::args()` ile
erişilir (bkz. [Bölüm 10](#10-native-builtinler-dil-çekirdeğine-gömülü) ve
[Bölüm 14](#14-stdlibos--komple-api-referansı)).

---

## 2. Dilin temelleri

### Yorumlar

```proton
// tek satır yorum
```

### Değişkenler: `var` / `const`

```proton
var age: int = 25;        // yeniden atanabilir
const PI_ISH: float = 3.14; // yeniden atanamaz

age = 26;                 // OK
// PI_ISH = 3.15;         // derleme hatası
```

**Önemli fark — global vs local:**

- **Global** (fonksiyon dışında tanımlanan) `var`/`const` yalnızca bir
  **literal** ile başlatılabilir: `const MAX: int = 100;` olur ama
  `const MAX: int = compute();` olmaz. Derleyici global başlangıç
  değerlerini derleme zamanında doğrudan VM'in global tablosuna yazıyor;
  bir "script gövdesi" çalıştırmıyor.
- **Local** (bir fonksiyon gövdesi içindeki) `var`/`const` herhangi bir
  ifadeyle başlatılabilir: `var total: int = sumTo(10) + 5;` gibi.

### Tip anotasyonu her zaman gerekli mi?

Evet — `var`/`const` bildirimlerinde ve fonksiyon parametrelerinde tip
anotasyonu zorunlu (`var x = 5;` gibi çıkarımlı/inferred bir sözdizimi yok).

### String'ler

```proton
var s: string = "merhaba";
var multi: string = """
çok satırlı
string
""";
var withEscape: string = "satır1\nsatır2\t\"tırnaklı\"\\";
```

Birleştirme (`+`) çalışır:

```proton
var full: string = "merhaba" + " " + "dünya";
```

Karakter indeksleme de çalışıyor (`s[i]`, tek karakterlik yeni bir string
döner — bkz. [Bölüm 12](#12-stdlibstring--komple-api-referansı)):

```proton
io::out("hello"[1]);  // "e"
```

### `io::` — girdi/çıktı

```proton
io::out("değer:", 42, " ve ", true);  // art arda yazdırır + \n
var line = io::in();                    // stdin'den bir satır okur;
                                         // sayıya çevrilebiliyorsa sayı,
                                         // değilse string döner
```

`io` gerçek bir `use`-yüklü modül **değil** — derleyiciye gömülü bir
sözde-namespace, `use io;` yazmaya gerek yok.

### `assert` ve `panic`

```proton
assert(1 + 1 == 2);          // koşul yanlışsa çalışma zamanı hatasıyla durur
panic("beklenmedik durum");  // koşulsuz, anlık program sonlandırma
```

---

## 3. Tip sistemi

Proton'un "mantıksal" bir tip sistemi var: **çalışma zamanında her sayı
`double` olarak tutuluyor** (int64/uint64 istisna — tam 64-bit saklanıyor,
aşağıya bkz.), ama her `var`/`const`/parametre için deklare edilen tip
**denetleniyor**: aralık dışı bir değer atarsan anlamlı bir hata alırsın.

### Tam tip listesi

| Tip | Açıklama | Eski takma ad |
|---|---|---|
| `bool` | `true`/`false` | |
| `char` | tek karakter / kod noktası | |
| `string` | metin | |
| `byte` | 0-255 | |
| `int8` | -128..127 | |
| `int16` | | `short` |
| `int32` | | `int` |
| `int64` | tam 64-bit hassasiyetli | `long` |
| `uint` | işaretsiz | |
| `uint8` | 0-255 | |
| `uint16` | | |
| `uint32` | | |
| `uint64` | tam 64-bit hassasiyetli | |
| `float32` | | `float` |
| `float64` | | `double` |
| `decimal` | | |
| `fn` | first-class fonksiyon değeri (denetlenmez) | |
| `T[]` | dizi/list (bkz. Bölüm 7) | |

```proton
var a: int8 = 120;
a = 200; // PANIC: int8 aralığı -128..127, 200 dışarıda
```

### `int64`/`uint64` — NumKind

Normal sayılar `double` olarak tutulduğu için ±2^53-1'in ötesinde hassasiyet
kaybederler. `int64`/`uint64` tipiyle deklare edilen değişkenler bu
sınırlamayı **saklama** açısından aşıyor (tam 64-bit temsil ediliyor, gerçek
`INT64_MAX`/`UINT64_MAX` doğru karşılaştırılıp yazdırılabiliyor):

```proton
const bigI: int64 = 9223372036854775807; // INT64_MAX -- tam hassasiyetle saklanır
var a: int64 = 9223372036854775807;
var b: int64 = 9223372036854775806;
io::out(a != b); // true -- double olsaydı bu ikisi ayırt edilemeyebilirdi
```

Kısıtlama sadece **aritmetik sonrası**: `+ - * /` sonucu hâlâ double'a
indirgeniyor, yani bir aritmetik işlemin sonucunu doğrudan bir `int64`
değişkene geri atamak, sonuç 2^53-1'i aşarsa hassasiyet kaybına/tip
uyumsuzluğuna yol açabilir. Basit `var`/`const` ataması ve karşılaştırma
işlemleri için güvenlidir.

### Generic'ler

```proton
fn max<T>(a: T, b: T): T {
    if (a > b) { return a; }
    return b;
}

fn main() {
    io::out(max<int>(3, 5));          // 5
    io::out(max<float64>(1.5, 2.5));  // 2.5
}
```

Generic struct'lar da var:

```proton
struct Box<T> {
    value: T;
}

fn main() {
    var b: Box = Box<int>{ value = 42; };
    io::out(b.value); // 42
}
```

**Önemli:** Generic tip argümanları **çıkarımlı (inferred) değil**, her
çağrı yerinde açıkça yazılmalı: `max<int>(3, 5)`, `max(3, 5)` değil.

---

## 4. Kontrol akışı

### `if` / `else`

```proton
if (age >= 18) {
    io::out("Yetişkin");
} else if (age >= 13) {
    io::out("Genç");
} else {
    io::out("Çocuk");
}
```

### `while`

```proton
var n: int = 5;
while (n > 0) {
    io::out("geri sayım: ", n);
    n--;
}
```

### `for` (klasik üç parçalı)

```proton
for (var i: int = 0; i < 10; i++) {
    if (i == 4) { continue; }
    if (i > 8) { break; }
    io::out(i);
}
```

### `for ... in` (dizi üzerinde gezinme)

```proton
var xs: int[] = [10, 20, 30];
for (x in xs) {
    io::out(x);
}
```

### `switch` / `case` / `default`

**Dikkat: C tarzı fall-through var.** Bir `case`'in sonunda `break` yoksa
yürütme bir sonraki case'e **düşmeye devam eder** (Swift/Rust'taki gibi
otomatik durmuyor):

```proton
switch (day) {
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
        io::out("Hafta içi");
        break;
    case 6:
    case 7:
        io::out("Hafta sonu");
        break;
    default:
        io::out("Geçersiz gün");
}
```

### `defer`

Fonksiyon gövdesinde, her `return`'den hemen önce çalışır. Birden fazla
`defer` **LIFO** (son tanımlanan ilk çalışır) sırayla işler:

```proton
fn demo(): void {
    defer { io::out("1. defer (son yazılan, ilk çalışan)"); }
    defer { io::out("2. defer"); }
    io::out("gövde çalışıyor");
    return;
}
// çıktı sırası: "gövde çalışıyor", "2. defer", "1. defer"
```

---

## 5. Fonksiyonlar

```proton
fn add(a: int, b: int): int {
    return a + b;
}
```

- Dönüş tipi zorunlu (`void` dahil, gövde `return;`/`return;` kullanmıyorsa
  bile).
- Recursion serbestçe çalışır (fonksiyonlar isimle çağrılıyor, global
  tabloda arama yapılıyor — ileri referanslar sorun değil).

```proton
fn factorial(n: int): int {
    if (n <= 1) { return 1; }
    return n * factorial(n - 1);
}
```

### First-class fonksiyon değerleri

Fonksiyonlar bir değişkene atanabilir, parametre olarak geçirilebilir,
yeniden atanabilir. `fn` tip adı "bu bir fonksiyon değeri tutar" demek için
kullanılır (denetlenmez, sadece niyet belirtir):

```proton
fn square(x: int): int { return x * x; }
fn cube(x: int): int { return x * x * x; }

fn apply(f: fn, x: int): int {
    return f(x);
}

fn main() {
    io::out(apply(square, 5));   // 25
    var g: fn = square;
    io::out(g(7));               // 49
    g = cube;
    io::out(g(4));               // 64
}
```

**Kasıtlı olarak yok: closure/upvalue capture.** Bir fonksiyon değeri
yalnızca kod + isme işaret eder — tanımlandığı kapsamdaki hiçbir yerel
değişkeni yakalamaz. `collections::map/filter/reduce/find`'a geçirdiğin
callback'ler bu yüzden **state'siz** olmalı (yalnızca kendi parametrelerini
kullanabilir).

---

## 6. Struct ve enum

Yalnızca **üst düzeyde** (top-level, bir fonksiyon gövdesi içinde değil)
tanımlanabilirler.

### `struct`

```proton
struct Point {
    x: int;
    y: int;
}

fn main() {
    var p: Point = Point{ x = 3; y = 4; };
    io::out(p.x, ",", p.y);
}
```

Generic struct:

```proton
struct Pair<K, V> {
    key: K;
    val: V;
}

fn main() {
    var p: Pair = Pair<string, int>{ key = "age"; val = 30; };
    io::out(p.key, "=", p.val);
}
```

### `enum`

```proton
enum Color { Red, Green, Blue }              // 0, 1, 2 otomatik
enum StatusCode { OK = 200, NotFound = 404 } // manuel değerler

fn main() {
    io::out(Color::Green);       // 1
    io::out(StatusCode::OK);     // 200
}
```

Üye erişimi salt-okunur (atama yapılamaz).

---

## 7. Diziler (Lists) ve Map'ler

### Diziler

```proton
var nums: int[] = [1, 2, 3, 4];
io::out(nums[1]);    // 2
nums[1] = 99;         // indeks ataması bir ifade olarak da değer döner
io::out(len(nums));   // 4
```

**Diziler `[...]` literal'ıyla sabit boyutlu doğar** — `OP_SET_INDEX`
yalnızca var olan bir indekse yazabilir, listeyi büyütemez. `var b: T[] = a;`
listeyi **kopyalamaz**, aynı listeye ikinci bir referans verir (alias):

```proton
var a: int[] = [1, 2, 3];
var b: int[] = a;
b[0] = 99;
io::out(a[0]); // 99 -- a da değişti! (aynı ObjList)
```

Dinamik büyüme ve gerçek kopya için iki native built-in var:

```proton
var xs: float64[] = [];
push(xs, 1.0);
push(xs, 2.0);
io::out(len(xs));  // 2

var a: float64[] = [1.0, 2.0];
var b: float64[] = listCopy(a); // bağımsız kopya
b[0] = 99.0;
io::out(a[0]);  // 1 (a değişmedi)
```

### Map'ler

```proton
var m: map = {"a": 1, "b": 2};
io::out(m["a"]);   // 1
m["c"] = 3;         // yeni anahtar ekler
io::out(len(m));    // 3
```

Yalnızca **string literal** anahtarlar kabul edilir (`{x: 1}` gibi
ifade-anahtarlı map yok). `map` özel bir keyword değil — tanınmayan tip adı
olarak kabul edilip denetlenmeden geçiyor.

### Dizi/map'i fonksiyondan döndürmek güvenli mi?

Evet. LAM (bkz. Bölüm 9) bunu otomatik hallediyor — bir fonksiyon kendi
region'ında ürettiği bir listeyi `return` ederse, dışarı çıkarken otomatik
olarak çağıranın region'ına kopyalanır, dangling pointer riski yok:

```proton
fn makeList(): int[] {
    var xs: int[] = [1, 2, 3];
    return xs; // güvenli
}
```

---

## 8. Hata değerleri ve `?` operatörü

Proton'da exception/try-catch **yok**. Bunun yerine Rust/Go tarzı "hata
değeri + kısa devre operatörü" var:

- Bazı native built-in'ler (`fs::read`, `sys::exec`, `net::get`, ...)
  başarısız olduğunda normal bir değer yerine bir **hata değeri**
  (`VAL_ERROR`) döner.
- **`expr?` (postfix `?`)**: `expr`'i çalıştırır, sonuç hata ise **o anki
  fonksiyonu erken sonlandırır** ve hatayı çağırana, bir `return` değeriymiş
  gibi iletir. Hata değilse yürütme normal devam eder. Zincirleme yayılır —
  `main`'e kadar yakalanmazsa "Uncaught error: ..." ile program durur.

```proton
fn readConfig(): string {
    var content: string = fs::read("config.txt")?; // hata varsa burada erken döner
    return content;
}

fn main() {
    var cfg: string = readConfig()?; // burada da yayılır, dosya yoksa
                                       // "Uncaught error: ..." ile durur
    io::out(cfg);
}
```

Şu anda hatayı programatik olarak inceleyip devam etme (`recover`) imkânı
yok — `?` yalnızca "hatasızsa devam et" / "hataysa yukarı fırlat" ikiliğini
sağlıyor.

`os.prt`, `collections.prt`, `string.prt` bu deponun stdlib'inde hata
değeri **üretmiyor** (pure Proton, panic ile duruyorlar) — hata değerleriyle
asıl karşılaşacağın yer `fs::`, `sys::exec`, ve `net::` native'leri.

---

## 9. Bellek modeli (LAM) — ne zaman önemli?

Proton'da GC yok; onun yerine **region-tabanlı deterministik bellek
yönetimi** var (LAM = Lifetime Allocation Model). Her fonksiyon çağrısı
kendi "region"ını (bump-pointer arena) alır; fonksiyon dönünce region
yıkılır.

**Günlük kullanımda bunu bilmen gerekmez** — derleyici/VM şu ikisini
otomatik hallediyor:

1. Bir fonksiyonun kendi region'ında ürettiği (concatenation, dizi
   literal'ı gibi) bir değeri `return` etmesi güvenlidir — otomatik olarak
   çağıranın region'ına "promote" edilir.
2. Bir üst kapsamdan gelen bir listeye/map'e/global'e o an çalışan
   frame'in kendi regional değerini yazmak (`OP_SET_INDEX`,
   `OP_BUILD_MAP`, global atama) da otomatik olarak doğru region'a
   promote ediliyor.

**Ne zaman fark eder?** Performans/bellek profili çıkarırken. Aynı
fonksiyonu milyonlarca kez çağırıp her seferinde ürettiğin ama return
**etmediğin** (kendi frame'inde tüketilen) geçici string/liste'ler kendi
region'larıyla birlikte anında temizlenir — bellek sabit kalır. Ama her
çağrıda **benzersiz bir string üretip return ediyorsan**, bu hâlâ kalıcı
belleğe "sızabilir" (Faz 1'in bilinen sınırı — escape analizi henüz yok).
Günlük kod yazarken bunu düşünmene gerek yok; yalnızca sıkı döngülerde
milyonlarca benzersiz string döndürüyorsan aklında olsun.

---

## 10. Native built-in'ler (dil çekirdeğine gömülü)

Bunlar `fn` ile tanımlanan kullanıcı fonksiyonları **değil** — derleyicide
özel olarak tanınan, doğrudan bytecode opcode'una giden sabit-arity
çağrılar. `use` gerektirmezler.

| Çağrı | Açıklama |
|---|---|
| `len(x)` | dizi/map/string uzunluğu, O(1) |
| `push(list, value)` | listenin sonuna ekler, listeyi döner |
| `listCopy(list)` | bağımsız kopya döner |
| `char::code(s)` | tek karakterlik string → kod (int, 0-255) |
| `char::fromCode(n)` | kod (0-255) → tek karakterlik string |
| `fs::read(path)` | dosya oku, string döner (hata → `VAL_ERROR`) |
| `fs::write(path, content)` | dosyaya yaz (üzerine), `nil` döner |
| `fs::exists(path)` | `bool`, O(1) `access()` kontrolü |
| `sys::exec(cmd)` | komutu subshell'de çalıştırır, stdout'u döner |
| `sys::env(name)` | ortam değişkeni oku, yoksa `nil` |
| `sys::setenv(name, value)` | **[yeni]** bu sürecin ortam değişkenini ayarlar |
| `sys::args()` | ekstra CLI argümanları, `string[]` |
| `sys::exit(code)` | **[yeni]** süreci hemen `code` ile sonlandırır, geri dönmez |
| `sys::pid()` | **[yeni]** bu sürecin pid'i, `int64` |
| `sys::ppid()` | **[yeni]** ebeveyn sürecin pid'i, `int64` |
| `time::now()` | Unix zamanı, ms (`int64`) |
| `time::ticks()` | monotonic mikrosaniye sayacı (`int64`) |
| `time::clock()` | süreç CPU süresi, saniye (`float64`) |
| `time::sleep(ms)` | `ms` milisaniye uyur |
| `time::format(ts, fmt)` | timestamp → biçimlendirilmiş string |
| `time::parse(dateStr, fmt)` | string → timestamp (`int64` ms), hata → `VAL_ERROR` |

> **[yeni]** işaretli dördü bu depoda bu oturumda eklendi (`OP_SYS_SETENV`,
> `OP_SYS_EXIT`, `OP_SYS_PID`, `OP_SYS_PPID`) — `stdlib/os.prt`'nin
> `set_env/exit/pid/parent_pid` fonksiyonları bunları sarmalıyor.

`net::` ailesi ayrı ve büyük olduğu için [Bölüm 18](#18-net--ağ-kütüphanesi)'de.

---

## 11. Modül sistemi (`use`)

```proton
use math;              // stdlib/math.prt'yi yükler, math:: önekiyle erişilir
use math as m;         // alias -- artık yalnızca m:: geçerli, math:: değil
```

- Derleyici `stdlib/<ad>.prt` dosyasını arar, üst düzey tanımları
  `<ad>.` önekiyle mangled bir global isimle derler. Çağrı yerinde
  `math::sqrt(2.0)` gibi gerçek noktalı erişim çalışır.
- Aynı modül birden fazla `use` edilirse yeniden yüklenmez (idempotent).
- Var olmayan bir modül (`use mathh;`) derleme hatası verir.
- **`private fn`/`private var`/`private const`**: modülün dışından
  `modül.üye` erişimini engeller; modülün kendi içinden erişim serbest.
- `io` bir modül değildir, `use io;` gerekmez.

Kendi modülünü eklemek istersen: `stdlib/<isim>.prt` dosyası oluştur,
üst düzey `fn`/`var`/`const` tanımla, `use <isim>;` ile yükle.

---

## 12. `stdlib/string` — komple API referansı

`use string;` ile yüklenir. Tüm fonksiyonlar `string::` önekiyle çağrılır.
Kaynak: `stdlib/string.prt` (219 satır, pure Proton). Test: `examples/test_string_stdlib.prt`.

Proton string'leri **immutable**; her fonksiyon yeni bir string (ya da
string listesi) döner, hiçbiri yerinde mutasyon yapmaz. Karakter erişimi
`s[i]` (native `OP_GET_INDEX`) ve `char::code`/`char::fromCode` üzerine
kurulu.

| İmza | Açıklama | Örnek |
|---|---|---|
| `string::len(s: string): int` | uzunluk (native `len()`'in ince sarmalayıcısı) | `string::len("hello")` → `5` |
| `string::upper(s: string): string` | büyük harfe çevirir (yalnızca a-z) | `string::upper("Hi")` → `"HI"` |
| `string::lower(s: string): string` | küçük harfe çevirir (yalnızca A-Z) | `string::lower("Hi")` → `"hi"` |
| `string::trim(s: string): string` | baş/son boşluk/tab/CR/LF temizler | `string::trim("  hi  ")` → `"hi"` |
| `string::split(s: string, sep: string): string[]` | ayraca göre böler; `sep=""` ise karakter karakter böler | `string::split("a,b,c", ",")` → `["a","b","c"]` |
| `string::join(list: string[], sep: string): string` | listeyi ayraçla birleştirir | `string::join(["a","b"], "-")` → `"a-b"` |
| `string::replace(s, old, replacement)` | tüm `old` geçişlerini değiştirir | `string::replace("aXaXa","X","-")` → `"a-a-a"` |
| `string::contains(s, sub): bool` | alt string içeriyor mu | `string::contains("hello","ell")` → `true` |
| `string::starts_with(s, prefix): bool` | önek kontrolü | `string::starts_with("hello","he")` → `true` |
| `string::ends_with(s, suffix): bool` | sonek kontrolü | `string::ends_with("hello","lo")` → `true` |
| `string::substring(s, start, end): string` | `[start,end)` aralığı, sınırlar clamp'lenir | `string::substring("hello",1,3)` → `"el"` |
| `string::repeat(s, n): string` | `s`'yi `n` kez tekrarlar | `string::repeat("ab",3)` → `"ababab"` |
| `string::reverse(s): string` | ters çevirir | `string::reverse("abc")` → `"cba"` |
| `string::char_at(s, index): string` | tek karakter döner (`s[index]`'in sarmalayıcısı) | `string::char_at("hello",1)` → `"e"` |
| `string::indexOf(s, sub): int` | ilk geçiş indeksi, yoksa `-1` | `string::indexOf("hello world","world")` → `6` |

**Not:** `replace`'in üçüncü parametresinin adı `new` **değil**
`replacement` — `new` dilde ayrılmış bir kelime (rezerve edilmiş, ileride
`new`/`delete` için).

Komple örnek:

```proton
use string;

fn main() {
    var s: string = "  Hello, World!  ";
    io::out(string::trim(s));                          // "Hello, World!"
    io::out(string::upper(string::trim(s)));            // "HELLO, WORLD!"
    io::out(string::split("a,b,,c", ","));               // [a, b, , c]
    io::out(string::join(["x","y","z"], "/"));            // x/y/z
    io::out(string::contains("proton lang", "lang"));    // true
    io::out(string::replace("foo bar foo", "foo", "baz")); // baz bar baz
}
```

---

## 13. `stdlib/collections` — komple API referansı

`use collections;` ile yüklenir. Kaynak: `stdlib/collections.prt` (148
satır, pure Proton). Test: `examples/test_collections_stdlib.prt`.

**Tip notu:** Proton generic'leri açık instantiation gerektiriyor ve dizi
tipleri tek seviyeli (`T[]`, `T[][]` yok). Her fonksiyonun her eleman
tipinde çalışabilmesi için koleksiyon parametresi/dönüşü `list[]`
(eleman tipi denetlenmeyen bir dizi) olarak tipleniyor; callback'ler
sıradan first-class `fn` değerleri (bkz. Bölüm 5 — **state'siz olmalı**,
closure yok).

| İmza | Açıklama | Örnek |
|---|---|---|
| `collections::map(list: list[], f: fn): list[]` | her elemana `f` uygular, yeni liste döner | `collections::map(xs, double)` |
| `collections::filter(list: list[], f: fn): list[]` | `f(elem)` true olanları tutar | `collections::filter(xs, isEven)` |
| `collections::reduce(list: list[], f: fn, initial: list[]): list[]` | sol katlama (fold): `acc = f(acc, elem)` | aşağıya bkz. |
| `collections::find(list: list[], f: fn): list[]` | ilk eşleşen elemanı döner, yoksa `nil` | `collections::find(xs, isEven)` |
| `collections::contains(list: list[], value: list[]): bool` | doğrusal arama | `collections::contains(xs, 9)` |
| `collections::reverse(list: list[]): list[]` | ters çevrilmiş yeni liste | |
| `collections::sort(list: list[]): list[]` | artan sıralama (insertion sort, kopya üzerinde) | |
| `collections::unique(list: list[]): list[]` | yinelenenleri temizler, sırayı korur | |
| `collections::flatten(list: list[]): list[]` | tek seviye düzleştirir | `collections::flatten([[1,2],[3]])` → `[1,2,3]` |
| `collections::range(start: int, end: int, step: int): int[]` | `[start,end)` aralığı, `step` negatif olabilir | `collections::range(0,10,2)` → `[0,2,4,6,8]` |

Komple örnek:

```proton
use collections;

fn isEven(x: int): bool { return x % 2 == 0; }
fn timesTwo(x: int): int { return x * 2; }

fn main() {
    var xs: int[] = [5, 3, 1, 4, 1, 5, 9, 2, 6];

    io::out(collections::map(xs, timesTwo));      // [10,6,2,8,2,10,18,4,12]
    io::out(collections::filter(xs, isEven));      // [4,2,6]
    io::out(collections::find(xs, isEven));        // 4
    io::out(collections::sort(xs));                 // [1,1,2,3,4,5,5,6,9]
    io::out(collections::unique(xs));               // [5,3,1,4,9,2,6]
    io::out(collections::range(0, 10, 2));          // [0,2,4,6,8]
}
```

**`sort` nasıl çalışır?** Insertion sort, Proton'un native `<`/`>`
operatörlerine dayanıyor — yani karşılaştırılabilir her tip (sayılar,
string'ler) üzerinde çalışır, ama özel bir comparator fonksiyonu **kabul
etmiyor** (dil henüz operatör overload/comparator geçme desteklemiyor).

---

## 14. `stdlib/os` — komple API referansı

`use os;` ile yüklenir. Kaynak: `stdlib/os.prt` (119 satır, pure Proton +
native `sys::*` sarmalayıcıları). Test: `examples/test_os_stdlib.prt`.

| İmza | Açıklama | Nasıl çalışır |
|---|---|---|
| `os::platform(): string` | `"Linux"`, `"Darwin"`, ... | `sys::exec("uname -s")` |
| `os::arch(): string` | `"x86_64"`, `"arm64"`, ... | `sys::exec("uname -m")` |
| `os::cpu_count(): int` | mantıksal CPU sayısı | `sys::exec("nproc")` + manuel decimal parse |
| `os::hostname(): string` | makine adı | `sys::exec("hostname")` |
| `os::cwd(): string` | çalışma dizini | `sys::exec("pwd")` |
| `os::home(): string` | ev dizini | `sys::exec("echo $HOME")` |
| `os::env(name: string): string` | ortam değişkeni oku | `sys::env(name)` (native) |
| `os::args(): string[]` | ekstra CLI argümanları | `sys::args()` (native) |
| `os::set_env(name, value): void` | **[yeni]** bu sürecin ortam değişkenini ayarlar | `sys::setenv` (native, `setenv()`) |
| `os::exit(code: int): void` | **[yeni]** süreci hemen sonlandırır, geri dönmez | `sys::exit` (native, `exit()`) |
| `os::pid(): int` | **[yeni]** bu sürecin pid'i | `sys::pid` (native, `getpid()`) |
| `os::parent_pid(): int` | **[yeni]** ebeveyn sürecin pid'i | `sys::ppid` (native, `getppid()`) |

**Neden bazı fonksiyonlar `sys::exec`, bazıları doğrudan native?**
`sys::exec` her zaman **ayrı bir child subshell**'de çalışır (`popen`) —
o subshell içinde bir `export FOO=bar` yapmak çağıran Proton sürecini asla
etkilemez, bir pid okumak da subshell'in kendi pid'ini verir, sürecin
gerçek pid'ini değil. Bu yüzden `set_env`/`exit`/`pid`/`parent_pid` bu
oturumda VM'e eklenen dört yeni native opcode'a (`OP_SYS_SETENV`,
`OP_SYS_EXIT`, `OP_SYS_PID`, `OP_SYS_PPID` — bkz. Bölüm 10) dayanıyor;
diğerleri (`platform`, `arch`, ...) hâlâ POSIX komutlarını `sys::exec` ile
çalıştırıp çıktısını ayrıştırıyor, çünkü bu bilgiler zaten salt-okunur/
bilgi amaçlı ve subshell'de çalışmaları bir sorun teşkil etmiyor.

Komple örnek:

```proton
use os;

fn main() {
    io::out("platform:", os::platform(), os::arch());
    io::out("cpu_count:", os::cpu_count());
    io::out("pid:", os::pid(), " parent_pid:", os::parent_pid());

    os::set_env("MY_VAR", "42");
    io::out("MY_VAR =", os::env("MY_VAR"));

    io::out("bitiyorum, kod 3 ile");
    os::exit(3);
    io::out("buraya asla gelmez");
}
```

---

## 15. `stdlib/math` — komple API referansı

`use math;` ile yüklenir. Kaynak: `stdlib/math.prt` (136 satır, pure
Proton). Örnek: `examples/stdlib_demo.prt`, `examples/stdlib_edge_test.prt`.

| İmza | Açıklama |
|---|---|
| `math::PI` / `math::E` | `const float64` sabitler |
| `math::abs(x: float64): float64` | mutlak değer |
| `math::absInt(x: int64): int64` | tamsayı mutlak değer |
| `math::min(a, b): float64` / `math::max(a, b): float64` | |
| `math::clamp(x, lo, hi): float64` | `[lo,hi]` aralığına sıkıştırır |
| `math::lerp(a, b, t): float64` | doğrusal enterpolasyon |
| `math::degToRad(deg): float64` / `math::radToDeg(rad): float64` | |
| `math::pow(base: float64, exp: int32): float64` | tamsayı üs (negatif olabilir) |
| `math::sqrt(x): float64` | Newton-Raphson; `x<0` ise **panic** |
| `math::sin(x)` / `math::cos(x)` / `math::tan(x)` | Taylor serisi + açı indirgeme |
| `math::floor(x)` / `math::ceil(x)` / `math::round(x)` | |

```proton
use math;

fn main() {
    io::out("sqrt(2) =", math::sqrt(2.0));
    io::out("sin(PI/2) =", math::sin(math::PI / 2.0));
    io::out("pow(2,10) =", math::pow(2.0, 10));
}
```

**Dikkat:** `math::sqrt(-1.0)` çağırmak programı **panic** ile durdurur
(negatif kök yakalanabilir bir hata değeri değil, kasıtlı sert bir hata).

---

## 16. `stdlib/random` — komple API referansı

`use random;` ile yüklenir. Kaynak: `stdlib/random.prt` (43 satır, pure
Proton). LCG (linear congruential generator) tabanlı — Proton'da bitwise
operatörler olsa da (bkz. Bölüm 21), bu modül tarihsel olarak yalnızca
çarpım/toplama/mod ile yazıldı; **kriptografik olarak güvenli değil.**

| İmza | Açıklama |
|---|---|
| `random::seed(value: uint32): void` | üretici durumunu ayarlar |
| `random::next(): uint32` | ham 32-bit değer |
| `random::nextFloat(): float64` | `[0, 1)` aralığında |
| `random::nextInt(min: int32, max: int32): int32` | `[min, max]` aralığında (iki uç dahil) |
| `random::nextBool(p: float64): bool` | `p` olasılıkla `true` |

```proton
use random;

fn main() {
    random::seed(42); // deterministik/tekrarlanabilir sonuçlar için
    for (var i: int32 = 0; i < 3; i++) {
        io::out(random::nextInt(1, 100));
    }
}
```

---

## 17. `stdlib/ml` — komple API referansı

`use ml;` ile yüklenir (kendi içinde `use math;` yapıyor, ekstra `use`
gerekmez). Kaynak: `stdlib/ml.prt` (529 satır, pure Proton). Örnek:
`examples/ml_demo.prt`.

**Veri temsili:** Vektör = `float64[]`. Matris = `list[]` (her elemanı bir
`float64[]` satır olan liste — `float64[][]` sözdizimi yok).

### Vektör istatistikleri
`sum(xs)`, `mean(xs)`, `variance(xs)`/`sampleVariance(xs)`,
`stddev(xs)`/`sampleStddev(xs)`, `vecMin(xs)`, `vecMax(xs)`,
`correlation(xs, ys)` (Pearson).

### Vektör cebiri
`vecAdd(a,b)`, `vecSub(a,b)`, `vecScale(v,s)`, `dot(a,b)`, `norm(v)`.

### Matris cebiri
`zerosMatrix(rows,cols)`, `matRows(m)`, `matCols(m)`, `matAdd(a,b)`,
`matScale(m,s)`, `matTranspose(m)`, `matMul(a,b)`, `matVecMul(m,v)`.

### Aktivasyon fonksiyonları
`sigmoid(x)`, `relu(x)`, `tanh(x)`, `sigmoidVec(xs)`, `reluVec(xs)`.

### Regresyon
`linRegFit(xs, ys)` → `LinRegModel{ slope; intercept; }` (kapalı form),
`linRegPredict(model, x)`, `linRegRSquared(model, xs, ys)`;
`gdFit(xs, ys, learningRate, epochs)` (batch gradient descent, çok
değişkenli), `gdPredict`, `mse`.

### K-means
`nearestCentroid(point, centroids)`,
`kMeansFit(points, initialCentroids, iterations)`.

```proton
use ml;

fn main() {
    var xs: float64[] = [1.0, 2.0, 3.0, 4.0, 5.0];
    var ys: float64[] = [2.0, 4.0, 6.0, 8.0, 10.0];
    var model: LinRegModel = ml::linRegFit(xs, ys);
    io::out("slope=", model.slope, " intercept=", model.intercept);
    io::out("R^2=", ml::linRegRSquared(model, xs, ys));
}
```

---

## 18. `net::` — ağ kütüphanesi

Bu bir `stdlib/*.prt` modülü **değil** — derleyiciye gömülü native
`net::` namespace'i (`use net;` yazmaya gerek yok). Senkron, düz-HTTP
(TLS yok).

| Çağrı | Açıklama |
|---|---|
| `net::get(url)` | GET, yanıt gövdesi (`string`), hata → `VAL_ERROR` |
| `net::post(url, body)` | POST, yanıt gövdesi |
| `net::request(options: map)` | esnek istek: `url`(zorunlu), `method`, `body`, `timeout`(ms, varsayılan 10000), `headers`(map). Döner: `{status, body, headers}` |
| `net::resolve(hostname)` | DNS lookup → ilk IP |
| `net::ping(host, timeoutMs)` | port 80'e TCP connect, süre (ms) ya da `-1` |
| `net::urlEncode(str)` / `net::urlDecode(str)` | percent-encoding |
| `net::serve(port, handler)` | blocking HTTP/1.1 sunucusu; `handler` bir `fn` değeri |
| `net::connect(host, port, protocol)` | outbound TCP/UDP, `handle` (int) döner |
| `net::send(handle, data)` | byte gönderir |
| `net::recv(handle, maxBytes)` | okur (1 ile 1 MiB clamp) |
| `net::close(handle)` | kapatır, no-op eğer zaten kapalıysa |

**Kasıtlı olarak yok:** `bind`/`listen`/`accept` — script asla dinleyen bir
soket açamaz (yalnızca `net::serve` istisna, ama o da VM'in kendi kontrolü
altında).

```proton
fn appHandler(req: map) {
    return { "status": 200, "body": "{\"ok\": true}" };
}

fn main() {
    net::serve(8080, appHandler);
}
```

---

## 19. Sık yapılan hatalar ve tuzaklar

1. **Global `var`/`const`'a fonksiyon çağrısı ile ilk değer vermek.**
   ```proton
   const X: int = compute(); // HATA -- global init yalnızca literal olabilir
   ```
   Çözüm: `main()` içinde `var`/`const` kullan, ya da fonksiyonu çağırmadan
   önce global'i literal ile başlatıp sonra `main` içinde ata.

2. **Dizi kopyalamayı `var b = a;` ile beklemek.**
   ```proton
   var b: int[] = a; // alias! kopya değil
   ```
   Çözüm: `listCopy(a)` kullan.

3. **`replace`'te üçüncü parametre adı olarak `new` yazmak.** `new`
   rezerve kelime, `stdlib/string.prt` bu yüzden `replacement` adını
   kullanıyor. Kendi kodunda da `new`, `double`, `char`, `string`, `int`
   gibi tip adlarını değişken/parametre adı olarak kullanamazsın.

4. **`switch`'te `break` unutmak.** Fall-through C gibi, otomatik durmuyor.

5. **`collections::` callback'lerinde closure beklemek.** Callback'ler
   state'siz first-class fonksiyonlar — dışarıdan bir değişkeni
   "yakalayamazlar". Gerekiyorsa değeri parametre olarak geçir (örn.
   `reduce`'un `initial` argümanı gibi).

6. **`math::sqrt(-1.0)` gibi çağrıların hata değeri değil panic
   olduğunu unutmak.** `?` operatörü burada işe yaramaz — `math::sqrt`
   `VAL_ERROR` değil gerçek bir `panic()` çağırıyor.

7. **Generic çağrılarda tip argümanını yazmayı unutmak.** `max(3,5)`
   çalışmaz, `max<int>(3,5)` gerekir.

8. **`sys::exec` ile ortam değişkeni ayarlamaya çalışmak.**
   `sys::exec("export X=1")` işe yaramaz (subshell'de kalır, ana sürece
   yansımaz) — bunun yerine `sys::setenv`/`os::set_env` kullan.

---

## 20. Alıştırmalar

Aşağıdakiler dilin farklı katmanlarını pekiştirmek için tasarlandı;
zorluk artan sırada. Her biri `stdlib/`'deki gerçek fonksiyonlarla
çözülebilir.

1. **Isınma:** `io::out`, `var`, `if/else` kullanarak bir sayının tek mi
   çift mi olduğunu yazdıran bir program yaz.
2. **Döngüler:** `for` ile 1'den 100'e kadar 3'e ya da 5'e bölünen
   sayıların toplamını hesapla.
3. **Fonksiyonlar + recursion:** Fibonacci'nin n. terimini hesaplayan
   recursive bir `fn` yaz, `assert` ile birkaç bilinen değeri doğrula.
4. **`string::`:** Kullanıcıdan `io::in()` ile bir cümle al, kelime
   sayısını (`string::split` + `len`), palindrom olup olmadığını
   (`string::reverse` + `==`) yazdır.
5. **`collections::`:** Bir `int[]` üzerinde `collections::filter` +
   `collections::map` zincirlemesi kullanarak "önce çift sayıları seç,
   sonra karesini al" işlemini yap.
6. **`collections::reduce`:** `reduce` kullanarak bir `float64[]`'in
   toplamını ve (ayrı bir çağrıda) maksimumunu hesapla — `ml::sum`'a
   bakmadan.
7. **Struct + generics:** Kendi `Stack<T>` struct'ını (bir `T[]` alanıyla)
   tasarla, `push`/`pop` benzeri iki fonksiyon yaz (native `push` ve dizi
   indekslemeyi kullan).
8. **Hata değerleri:** `fs::write` ile bir dosyaya yaz, sonra `fs::read(...)?`
   ile geri oku ve `io::out` et; dosya yolunu kasten bozup hatanın
   `main`'e kadar yayıldığını gözlemle.
9. **`os::` + `sys::`:** `os::platform()`, `os::cpu_count()`,
   `os::pid()`'i yazdıran, sonra `os::set_env` ile bir değişken ayarlayıp
   `os::env` ile geri okuyan bir "sistem bilgisi" scripti yaz.
10. **`ml::`:** Kendi ürettiğin (`random::nextFloat` ile) sentetik bir
    `xs`/`ys` veri kümesi üzerinde `ml::linRegFit` çalıştır, `R²`'yi
    yazdır.

---

## 21. Hızlı komut/opcode kartı

Bu tablo derleyicide **özel olarak tanınan** (yani kullanıcı `fn`'i
olmayan) tüm sözde-namespace ve fixed-arity built-in'leri özetler.

| Namespace/çağrı | Üyeler |
|---|---|
| `io::` | `out(...)`, `in()` |
| `char::` | `code(s)`, `fromCode(n)` |
| `fs::` | `read(path)`, `write(path,content)`, `exists(path)` |
| `sys::` | `exec(cmd)`, `env(name)`, `args()`, `setenv(name,value)`, `exit(code)`, `pid()`, `ppid()` |
| `time::` | `now()`, `ticks()`, `clock()`, `sleep(ms)`, `format(ts,fmt)`, `parse(dateStr,fmt)` |
| `net::` | `get`, `post`, `request`, `resolve`, `ping`, `urlEncode`, `urlDecode`, `serve`, `connect`, `send`, `recv`, `close` |
| serbest (namespace'siz) | `len(x)`, `push(list,v)`, `listCopy(list)`, `assert(expr)`, `panic(msg)` |

Ve `use`-yüklü, pure-Proton stdlib modülleri (`stdlib/*.prt`):

| Modül | `use` | Öne çıkan üyeler |
|---|---|---|
| `math` | `use math;` | `PI`, `E`, `sqrt`, `sin/cos/tan`, `pow`, `clamp`, `floor/ceil/round` |
| `random` | `use random;` | `seed`, `next`, `nextFloat`, `nextInt`, `nextBool` |
| `ml` | `use ml;` | vektör/matris istatistikleri, regresyon, k-means |
| `string` | `use string;` | `upper/lower/trim/split/join/replace/contains/substring/...` |
| `collections` | `use collections;` | `map/filter/reduce/find/sort/unique/flatten/range` |
| `os` | `use os;` | `platform/arch/cpu_count/hostname/cwd/home/env/args/set_env/exit/pid/parent_pid` |

**Operatör öncelik sırası (düşükten yükseğe):**
`||` < `&&` < `|` < `^` < `&` < eşitlik (`== !=`) < karşılaştırma
(`< <= > >=`) < shift (`<< >>`) < toplama/çıkarma (`+ -`) < çarpma/bölme/mod
(`* / %`) < unary (`! - ~`) < postfix (`?`, `++`, `--`, `[]`, `()`, `.`).

---

*Bu döküman `README.md`'nin teknik/mimari odaklı içeriğini tekrar etmez —
oradaki "LAM", "NumKind", ve değişiklik günlüğü bölümlerine referans
verir. İkisi birlikte okunmalı: bu döküman "nasıl yazarım", `README.md`
"nasıl çalışır" sorusuna cevap veriyor.*
