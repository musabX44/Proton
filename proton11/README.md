# Proton — C interpreter (v11, stack-based VM)

Tek geçişli (single-pass) Pratt parser + register-free stack tabanlı bytecode VM.
Mimari `clox` (Crafting Interpreters) tarzına yakın: kaynak kod doğrudan bytecode'a
derleniyor (ayrı bir AST katmanı yok), sonra VM bu bytecode'u koşuyor.

**Proton 11 notu:** Bu sürüm, dil sözdiziminde/semantiğinde hiçbir
değişiklik yapmadan VM'in iç bellek yönetimine üç performans
optimizasyonu ekler (region pooling, lazy block allocation, döngü-scope
region rewind). Son optimizasyon, derleyicideki hafif bir statik
escape analizi ile korunuyor — ayrıntı için aşağıdaki
**"✅ Proton 11: Döngü-Scope Rewind — Statik Escape Analizi ile
Düzeltildi"** bölümüne bakın.

## Derleme

```sh
make
./proton examples/hello.prt
./proton examples/factorial.prt
```

## Mimari

```
src/lexer.c     -> kaynak metni token'lara çevirir (yorum satırları, çok satırlı
                    string, escape karakterleri destekleniyor)
src/compiler.c  -> tek geçişli Pratt parser; doğrudan bytecode üretir
src/chunk.c     -> bytecode + sabitler (constants) havuzu
src/value.c     -> Value tipi (nil / bool / number / obj)
src/object.c    -> heap nesneleri: ObjString (interned), ObjFunction
src/table.c     -> açık adresleme (open addressing) hash tablosu
src/vm.c        -> bytecode yorumlayıcı (call frame'ler, stack)
src/main.c      -> CLI giriş noktası
```

### Tasarım kararları

- **Fonksiyonlar isimle çağrılıyor.** `OP_CALL` çalışma zamanında global tabloda
  isme göre arama yapıyor. Bu sayede ileri referanslar (bir fonksiyonun kendinden
  sonra tanımlanan başka bir fonksiyonu çağırması) ve recursion otomatik çalışıyor,
  ayrı bir "forward declaration" mekanizmasına gerek kalmıyor.
- **Global `var`/`const` sadece literal (sabit) başlangıç değeri alabiliyor**
  (`const MAX_USERS: int = 100;` gibi). Bunun sebebi: global bytecode çalıştıran
  bir "script" fonksiyonu yerine, derleyici bu değerleri doğrudan derleme
  zamanında VM'in global tablosuna yazıyor. İleride gerekirse bir top-level
  script chunk'ı eklenip bu kısıtlama kaldırılabilir.
- **Sayılar (int/float) çalışma zamanında `double` olarak tutuluyor.** Tip
  anotasyonları (`: int`, `: float` vs.) parse ediliyor ve artık **denetleniyor**
  da: her primitive tip (`int8`..`uint64`, `float32/64`, `byte`, `char`,
  `bool`, `string`, ...) declare/assign/parametre noktalarında `OP_CHECK_TYPE`
  ile (global literal init'lerde derleme zamanında) doğrulanıyor. Runtime
  temsili hâlâ tek bir `double` olduğu için bu "mantıksal" bir tip sistemi —
  gerçek bellek boyutu/performans farkı (örn. gerçek 1 byte'lık `uint8`
  depolama) için `Value`'ya ayrı tag'ler eklemek gerekir.
- **`break`/`continue`**, derleyicinin döngü bağlamı (loop context) yığınıyla
  ve döngüye girildiğinden beri açılan local değişken sayısına göre otomatik
  `OP_POP` enjekte ederek doğru stack dengesini koruyor.

## LAM (Lifetime Allocation Model) — Faz 1

GC yerine, region-tabanlı deterministik bellek yönetimi. Faz 1 kapsamı
bilinçli olarak dar tutuldu: **yalnızca çalışma zamanında üretilen
string'ler** (concatenation sonucu, sayı/bool → string dönüşümü)
region'lı; string literal'lar (kaynak kodda yazılanlar) eskisi gibi
intern edilip kalıcı kalıyor.

### Mimari

- **Granularity: per-call-frame, per-lexical-block değil.** Her
  `CallFrame` kendi `Region`'ını taşıyor (`src/region.c`: bump-pointer
  arena, blok zincirlemeli). `OP_CALL` region'ı yaratıyor, `OP_RETURN`
  frame pop edilirken koşulsuz yıkıyor. Bu tercih bilinçli: per-block
  (her `{ }` girişi/çıkışı) ayrı region istemek, her `return`/`break`/
  `continue` noktasında doğru temizliği garantilemek için escape
  analysis gerektirirdi — Faz 1'de bu yok, o yüzden VM'in zaten tam
  kontrol ettiği tek ömür sınırı (call frame) kullanıldı.
- **String interning ile region'lar birbirinden tamamen ayrık.**
  `copyString`/`takeString` (literal'lar) hep kalıcı heap'te, hiçbir
  region'a bağlı değil — bu sayede iki farklı scope aynı intern edilmiş
  string'i güvenle paylaşabiliyor. `regionTakeString` (yeni) ise hiçbir
  zaman intern tablosuna girmiyor; `ObjString.isRegional` bayrağı bunu
  işaretliyor.

### Bilinen sınır: fonksiyon dönüş değeri olarak string escape'i

Faz 1'de escape analizi yok. Bir fonksiyon kendi region'ında ürettiği
bir string'i `return` ederse (`return a + b;` gibi), o region
`OP_RETURN`'de yıkılmadan önce dönüş değeri **kalıcı olarak intern
edilir** (`vm.c`, `OP_RETURN`) — aksi halde çağıran taraf dangling
pointer alırdı. Bu doğru ama muhafazakâr bir çözüm: fonksiyonundan her
zaman **yeni, benzersiz** bir string döndüren bir fonksiyon çok sayıda
çağrıldığında (örn. bir döngüde `build(i)` gibi her seferinde farklı bir
string üreten bir fonksiyon), bu promosyon kalıcı belleği yine
büyütür — çünkü hangi çağıranın o string'i hâlâ tutacağını bilmenin
tek yolu escape analizi, o da Faz 2.

Fonksiyonun tükettiği ama dışarı **döndürmediği** geçici string'ler
(örn. `describe(n-1)`'in ara sonucu, kendi frame'i içinde kalıp dışarı
çıkmıyorsa) bu sorunu yaşamaz — region'ları ile birlikte temizlenirler.
Sorun yalnızca "her çağrıda farklı bir string üret ve return et" deseninde
ortaya çıkar.

### Ölçüm

`examples/lam_stress_unique.prt` (200k çağrı, her çağrıda benzersiz
string, hiçbiri return edilmiyor — hepsi kendi frame'inde tüketiliyor):

| | Peak RSS |
|---|---|
| Faz 1 öncesi | 72 MB (1M yinelemede 334 MB) |
| Faz 1 sonrası | 2.09 MB (1M yinelemede de 2.09 MB, sabit) |

### Faz 2'ye bırakılanlar

- Escape analizi: bir string'in `return` ile mi, bir global'e atanarak
  mı, yoksa bir struct alanına konarak mı (struct'lar eklenince) dışarı
  kaçtığını tespit edip, kaçmıyorsa region'da bırakmak.
- Per-lexical-block granularite (bir `{ }` bloğu bitince, henüz
  fonksiyon dönmeden, o bloğa özel region'ı yıkmak).
- Dizi/struct eklenince onların da region/heap ayrımı.

### ✅ Proton 11: Döngü-Scope Rewind — Statik Escape Analizi ile Düzeltildi

Proton 11, performans/bellek optimizasyonu olarak her `OP_LOOP`'ta
(bir `while`/`for`/`continue` turu tamamlanıp başa dönüldüğünde) o
frame'in region'ının bump-pointer'ını **o iterasyonun başındaki
konuma geri sarar** (`regionRewind`, bkz. `src/region.c` ve
`src/vm.c`'deki `OP_LOOP` case'i). Bu, sıkı döngülerde çok sayıda
kısa ömürlü string/list üreten kodun belleğinin fonksiyon dönene kadar
değil, **her iterasyonda** geri kazanılmasını sağlar — 3 milyon
iterasyonluk bir string-concat benchmark'ında peak RSS'i 392 MB'tan
2.1 MB'a indirdi.

**Bu optimizasyon artık derleyicideki hafif bir statik escape analizi
ile korunuyor.** Derleyici (`src/compiler.c`), bir döngü gövdesini
derlerken, döngüden **önce** tanımlanmış bir local değişkene yapılan
her atamayı izler (`markLoopEscapeIfLocalPredatesLoop`,
`LoopCtx.escapes`). Böyle bir atama tespit edilirse — tam olarak
"döngü içinde üret → döngü içindeki bir dış değişkene ata" deseni —
o döngüye ait **her** geri-kenar (`OP_LOOP` — gövdenin kendisi,
`continue`, `for`'un artış ifadesi dahil) bir bayrakla işaretlenir.
VM bu bayrağı gördüğünde o geri-kenarda `regionRewind`'i **atlar**;
region yalnızca fonksiyon dönüşünde (`OP_RETURN`) temizlenir, tıpkı
Faz 1'in orijinal (rewind'siz) davranışı gibi. Bayrak yoksa (döngü
içinde üretilen hiçbir değer döngü dışına atanmıyorsa) rewind eskisi
gibi koşulsuz çalışmaya devam eder ve tam performans/bellek kazancı
korunur.

Bu analiz **muhafazakâr**dır: yalnızca "hedef local'in slot'u, içinde
bulunulan (veya onu saran) döngünün başlangıcından önce mi
tanımlanmış" sorusuna bakar — değerin gerçekten döngü sonrasında
okunup okunmadığını izlemez. Yanlış pozitif (gereksiz yere rewind'i
kapatmak) mümkündür ama her zaman güvenli taraftadır; yanlış negatif
(sessizce yanlış sonuç) yoktur. `OP_SET_GLOBAL` ve `OP_SET_INDEX`
(map/list eleman ataması) zaten ayrı bir mekanizmayla
(`promoteRegionalValue`) her zaman kalıcı depoya taşınıyordu, bu
yüzden escape analizi yalnızca local-to-local atamaya odaklanıyor.

Canlı doğrulama: `examples/lam_rewind_unsound_demo.prt` artık doğru
sonucu veriyor:

```
saved (should still be item-2) = item-2
```

Escape içermeyen döngülerde (örn. `examples/lam_stress_test.prt`,
200k çağrı, hiçbir değer döngü dışına taşmıyor) peak RSS hâlâ ~2.1 MB
— rewind optimizasyonu bu durumda tam performansıyla çalışmaya devam
ediyor, hiçbir regresyon yok.



## NumKind — int64/uint64 tam hassasiyet

`Value`'nun sayı alanı artık `{NumKind, union{f64,i64,u64}}` şeklinde
etiketli — düz `double` değil. `NUM_F64` etiketsiz/float sayılar için
varsayılan (önceki davranışla birebir aynı); bir değer `int64`/`uint64`
tipiyle `OP_CHECK_TYPE`'tan geçtiğinde `NUM_I64`/`NUM_U64` olarak
işaretlenip gerçek 64-bit temsiliyle saklanıyor. Böylece `int64`/`uint64`
değişkenler artık double'ın kesin-tamsayı sınırının (±2^53-1) ötesinde de
hatasız karşılaştırılıp yazdırılabiliyor. Aritmetik (`+ - * /`, karşılaştırma
operatörleri) hâlâ `AS_NUMBER()` üzerinden double'a indirgeniyor — yani
karışık/int64 aritmetiği hâlâ double'a yükseltiliyor; gerçek 64-bit yerel
aritmetik sonraki bir adım.

## Şu an çalışan özellikler

- **`use <ad>;` gerçek bir modül/namespace sistemi.** Derleyici
  `stdlib/<ad>.prt` dosyasını arıyor (çalışma dizinine göre) ve dosyanın
  üst düzey tanımlarını **`<ad>.` ön ekiyle mangled bir global isimle**
  derliyor (`sqrt` → `math.sqrt`). Çağrı yerinde de gerçek noktalı erişim
  çalışıyor: `math.sqrt(2.0)`, `math.PI` — flat/düz isimlere düşürülmüyor.
  Aynı modül birden fazla `use` edilirse yeniden yüklenmiyor (idempotent).
  `io` bunun dışında: derleyiciye gömülü, dosyasız bir sözde-namespace
  (`io.out`/`io.in` özel olarak tanınıyor), `use io;` yazmaya gerek yok.
  - **Var olmayan modül artık bir derleme hatası.** `use mathh;` (yazım
    hatası) gibi durumlar `stdlib/mathh.prt` bulunamadığında anlamlı bir
    hatayla ("Module 'mathh' not found...") durur — önceden sessizce
    yutulup yalnızca ilk çağrı noktasında (`mathh.foo()`'da) belirsiz bir
    hataya yol açıyordu.
  - **Görünürlük kontrolü: `private fn` / `private var` / `private const`.**
    Bir modülün top-level `fn`/`var`/`const`'ını `private` işaretlemek,
    dışarıdan `modül.üye` şeklinde erişimi engelliyor (derleme hatası);
    modülün **kendi içinden** (bare isimle ya da kendi ön ekiyle nitelikli
    olarak) erişim serbest kalıyor. `stdlib/math.prt`'deki iç yardımcı
    `_reduceAngle` artık gerçekten `private` — bkz. `stdlib/math.prt`.
    Henüz `struct`/`enum` için değil, yalnızca `fn`/`var`/`const` için;
    `private` yalnızca bir modül gövdesi içinde anlamlı, top-level script
    içinde kullanılırsa hata verir.
  - **Import alias'ı: `use math as m;`.** Çağrı yerinde `m.sqrt(2.0)`
    gibi kısaltılmış bir isimle erişilebiliyor; mangled global anahtar
    hâlâ gerçek modül adını (`math.sqrt`) kullanıyor, yani aynı modülün
    farklı `use` noktalarında farklı alias'larla yüklenmesi aynı
    depolamaya işaret ediyor. Alias verildiğinde modülün gerçek adı
    (`math`) artık bir namespace olarak tanınmıyor — yalnızca alias
    geçerli (Python'daki `import x as y` semantiğine benzer).
- **`stdlib/` altında saf Proton ile yazılmış üç modül var**: `math.prt`
  (`PI`, `E`, `abs`, `absInt`, `min`, `max`, `clamp`, `lerp`, `pow`,
  `sqrt` [Newton-Raphson], `sin/cos/tan` [Taylor serisi + açı indirgeme,
  iç yardımcısı `_reduceAngle` artık `private`], `floor/ceil/round`,
  `degToRad/radToDeg`), `random.prt` (LCG tabanlı `seed/next/nextFloat/
  nextInt/nextBool` — Proton'da bitwise operatör olmadığı için xorshift
  değil, çarpım/toplama/mod tabanlı bir LCG), ve `ml.prt` (istatistik +
  lineer cebir + basit ML — bkz. aşağıdaki "`ml::` Kütüphanesi" bölümü).
  Tüm çağrılar noktalı: `math::sqrt(2.0)`, `random::nextInt(1, 100)`,
  `ml::mean(xs)`. Örnek kullanım: `examples/stdlib_demo.prt`,
  `examples/ml_demo.prt`.
- `var` / `const` (global: yalnızca literal init; local: herhangi bir ifade)
- **Primitive tipler ve tip denetimi**: `bool`, `char`, `string`, `byte`,
  `int8/16/32/64`, `uint`, `uint8/16/32/64`, `float32/64`, `decimal`, ve eski
  takma adlar (`short`→int16, `int`→int32, `long`→int64, `float`→float32,
  `double`→float64). Sayılar hâlâ runtime'da `double` olarak tutuluyor, ama
  her `var`/`const`/parametre tipine göre **derleme zamanında** (global
  literal init'ler) veya **çalışma zamanında** (`OP_CHECK_TYPE`, local init,
  her yeniden atama, `++`/`--`, ve fonksiyon parametreleri için) denetleniyor.
  Aralık dışı bir değer atanırsa (`var a: int8 = 200;` gibi) anlamlı bir hata
  ile durur. `int64`/`uint64` artık **saklama** açısından tam 64-bit hassasiyete
  sahip (bkz. yukarıdaki "NumKind" bölümü); kısıtlama sadece **aritmetik
  sonrası**: `+ - * /` sonucu hâlâ double'a indirgeniyor, bu yüzden bir
  aritmetik işlemin sonucunu doğrudan bir `int64` değişkene geri atamak tip
  hatası verebilir.
- `fn`, `return`, recursion
- `if` / `else`
- `while`, `for` (klasik üç parçalı), `break`, `continue`
- `switch` / `case` / `default` — **C tarzı fall-through**: bir case'in
  sonunda `break` yoksa yürütme bir sonraki case/default'a düşmeye devam
  eder (Swift/Rust'taki gibi otomatik durmuyor).
- `struct Name { field: type; ... }` — sadece **üst düzeyde (top-level)**
  tanımlanabiliyor, bir fonksiyon gövdesi içinde `struct`/`enum` tanımlamak
  hâlâ reddediliyor. Runtime'da `ObjMap` üzerinde temsil ediliyor (ayrı bir
  instance tipi yok). Generic struct'lar da çalışıyor: `Box<T>{ value = 42; }`.
- `enum Name { A, B, C }` (otomatik artan değerler) ve `enum Name { A, B = 5 }`
  (manuel/karışık değerler) — üye erişimi `Name.Member` şeklinde, salt-okunur
  (atama yok). Bu da sadece üst düzeyde tanımlanabiliyor.
- `defer { ... }` — yalnızca fonksiyon gövdesi içinde; bir fonksiyondaki
  birden fazla `defer` bloğu **LIFO** (son tanımlanan ilk çalışır) sırayla,
  her `return`'den hemen önce çalıştırılıyor.
- **Generics**: `fn max<T>(...)` ve `struct Box<T>` — tip argümanı sayısı
  yanlışsa veya tip uyuşmazlığı varsa anlamlı derleme/çalışma zamanı hatası;
  nested generic çağrılar destekleniyor.
- **Hata değerleri + `?` operatörü**: bkz. aşağıdaki "Hata değerleri" bölümü.
- Aritmetik: `+ - * / %`, karşılaştırma: `== != < <= > >=`, mantıksal: `&& || !`
  (kısa devre / short-circuit ile)
- `++` / `--` (yalnızca kendi başına bir ifade olarak: `age--;`, `i++`)
- `io.out(...)` (değişken sayıda argüman, art arda yazdırır + yeni satır)
- `io.in()` (stdin'den satır okur; sayıya çeviriebiliyorsa sayı, değilse string döner)
- `assert(expr);`, `panic(...);`
- String literal'lar: normal `"..."`, escape (`\n`, `\t`, `\"`, `\\`), ve
  çok satırlı `""" ... """`
- `string + string` birleştirme (concatenation)

## Diziler (Lists) ve Map'ler

Bu bölümde anlatılan özellikler kodda mevcut ama önceki README revizyonunda
belgelenmemişti.

- **`T[]` dizi tipi ve `[...]` literal'ı** artık çalışıyor:

  ```
  var nums: int[] = [1, 2, 3, 4];
  io.out(nums[1]);   // 2
  nums[1] = 99;      // indeks ataması bir ifade olarak da değer döner
  io.out(len(nums)); // 4
  ```

  Eleman tipi (`int[]` -> `int`) `ObjList->elemType` olarak saklanıyor ve
  literal oluşturma / `OP_SET_INDEX` sırasında `OP_CHECK_TYPE` ile
  denetleniyor (diğer primitive tipler gibi).
- **Map literal'ı `{ "anahtar": değer, ... }`** — yalnızca string literal
  anahtarlara izin veriliyor (`{ x: 1 }` gibi ifade-anahtarlı map yok).
  Aynı `[]` indeksleme sözdizimi map'ler için de geçerli:

  ```
  var m: map = {"a": 1, "b": 2};
  io.out(m["a"]);   // 1
  m["c"] = 3;       // yeni anahtar ekler
  io.out(len(m));   // 3
  ```

  Map'in kendi tipi için özel bir `map` anahtar kelimesi/keyword YOK;
  yukarıdaki `map` sadece "tanınmayan tip adı" olarak kabul edilip
  denetlenmeden geçiyor (tıpkı henüz var olmayan struct tipleri gibi). Map
  değeri hâlâ çalışıyor, sadece derleme zamanında "bu bir map'tir" diye ayrı
  bir tip denetimi yok.
- **`len(x)`** hem dizi hem map için eleman sayısını döndürüyor; O(1), hiçbir
  gezinme (traversal) yapmıyor.
- **Lifetime (ömür) — dizi/list'leri fonksiyondan `return` etmek artık
  güvenli.** `ObjList` LAM (bkz. yukarıdaki bölüm) kapsamında hâlâ
  **region-scoped** doğuyor, ama string'lerdeki `promoteEscapingValue`
  mekanizmasının bir benzeri artık dizilere de uygulanıyor
  (`regionCopyList` / `permanentCopyList`, `object.c`): `OP_RETURN`,
  döndürülen bir listenin sahibi olduğu region'ı yıkmadan önce, listeyi
  (ve içindeki her elemanı — iç içe listeler ve çalışma zamanında üretilen
  string'ler dahil, recursive olarak) bir üst (çağıranın) region'ına
  kopyalıyor. String'lerdeki gibi bu da zincirleme ilerliyor: değer, en
  dıştaki frame'e kadar hangi region'a taşınıyorsa orada kalıyor, yalnızca
  en dıştaki frame'i de aşarsa (nadir bir durum) kalıcı/malloc'lu depoya
  düşüyor. Yani artık:

  ```
  fn makeList(): int[] {
      var xs: int[] = [1, 2, 3];
      return xs;    // artık güvenli: xs, döner dönmez çağıranın region'ına kopyalanıyor
  }
  ```

  gibi bir kod hem derlenir hem de doğru ve tanımlı şekilde çalışır — dangling
  pointer riski yok. Map'ler zaten bu sorunu hiç yaşamıyordu (`ObjMap`
  kalıcı/malloc'lu, `newMap()`'e bakın); artık dizi/list'ler için de eşdeğer
  bir güvence var, sadece farklı bir mekanizmayla (region-chain promotion,
  malloc yerine). Not: bu yalnızca `return` ile kaçışı kapsıyor — bir
  fonksiyonun kendi frame'inde ürettiği regional bir string/liste'yi
  dönmeden bir üst kapsamdaki (örn. dışarıdan geçirilen) bir listenin
  içine `OP_SET_INDEX` ile yazmak (struct alanlarında da olacağı gibi) hâlâ
  ayrı, ele alınmamış bir escape yoludur — bu, tam escape analizinin (Faz
  2) kapsamında.

### `push(list, value)` ve `listCopy(list)`

Dizi/list literal'ları (`[...]`) sabit boyutlu doğuyor: `OP_SET_INDEX`
yalnızca **var olan** bir indekse yazabiliyor, listeyi büyütemiyor. Aynı
şekilde `var b: T[] = a;` bir listeyi kopyalamıyor, aynı `ObjList`'e
işaret eden ikinci bir referans veriyor (`Value` düz bir pointer taşıyor)
— yani `b[0] = 99;` demek `a[0]`'ı da değiştirir. Bu iki sınır, dinamik
büyüyen veri yapıları (matris satırları biriktirmek, gradyan inişte
ağırlık vektörü oluşturmak, vb.) yazmayı imkânsız kılıyordu, bu yüzden
iki yeni native built-in eklendi (`len(x)` ile aynı desende: derleyicide
özel tanınan, doğrudan opcode'a giden çağrılar, kullanıcı `fn`'i değil):

- **`push(list, value)`** — `value`'yu `list`'in sonuna ekler (mevcut
  `appendList` C fonksiyonu kullanılıyor — amortized O(1), kapasite
  dolunca region içinde 2x büyüyen bir arraya kopyalanıyor), ve **aynı
  listeyi** ifade değeri olarak geri döndürür (`OP_SET_INDEX`'in
  "atama da bir ifadedir" kuralıyla tutarlı). `value`, `list`'in kendi
  region'ına gerekirse promote ediliyor (bkz. yukarıdaki "Ömür" bölümü
  — `OP_SET_INDEX`'teki hazard'ın aynısı).
  ```
  var xs: float64[] = [];
  push(xs, 1.0);
  push(xs, 2.0);
  io::out(len(xs));   // 2
  ```
- **`listCopy(list)`** — `list`'in bağımsız, ayrıca mutasyona
  uğratılabilir bir kopyasını döner (çağıran frame'in region'ında,
  `regionCopyList` ile — iç içe liste/string elemanlar da recursive
  olarak promote ediliyor). `var b: T[] = a;`'nın bıraktığı alias
  boşluğunu tam olarak kapatıyor.
  ```
  var a: float64[] = [1.0, 2.0];
  var b: float64[] = listCopy(a);
  b[0] = 99.0;
  io::out(a[0]);   // 1  (a değişmedi)
  io::out(b[0]);   // 99
  ```

Bu ikisi olmadan sabit boyut/alias kısıtları, `ml::` gibi dinamik boyutlu
vektör/matris üreten herhangi bir stdlib modülünü imkânsız kılıyordu;
`stdlib/ml.prt`'nin neredeyse tüm fonksiyonları bu iki built-in'e dayanıyor.

## Hata değerleri (`VAL_ERROR`) ve `?` operatörü

Basit bir "recoverable error" mekanizması var — exception/try-catch değil,
Rust/Go tarzı "hata değeri + kısa devre operatörü":

- `fs_read(path)`, `fs_write(path, content)`, `fs_exists(path)`, `sys_exec(cmd)`
  gibi native (host tarafından sağlanan) built-in'ler başarısız olunca normal
  bir değer yerine bir **hata değeri** (`VAL_ERROR`) döndürüyor.
- **`expr?` (postfix `?` operatörü, `OP_TRY`)**: `expr`'i normal derler,
  sonucu çalışma zamanında kontrol eder — eğer bir hata ise, **o anki
  fonksiyonu erken sonlandırır** (kendi region'ını yıkar) ve hatayı
  çağıranın stack'ine, tıpkı bir `return` değeriymiş gibi, iletir. Hata
  değilse, değer stack'te olduğu gibi kalır ve yürütme normal devam eder.
  Zincirleme yayılır: `?` en dıştaki (`main`) çağrıya kadar hatayı taşır;
  orada yakalanmazsa "Uncaught error: ..." ile programı sonlandırır.

  ```
  fn readConfig(): string {
      var content: string = fs_read("config.txt")?; // hata varsa burada fonksiyon
                                                       // erken döner, hatayı taşır
      return content;
  }

  fn main() {
      var cfg: string = readConfig()?; // dosya yoksa burada da yayılır ve
                                         // "Uncaught error: Dosya okunamadi"
                                         // ile durur
      io.out(cfg);
  }
  ```

- Şu an gerçek bir `try { } catch (e) { }` bloğu YOK — hatayı "yakalamanın"
  tek yolu, henüz Proton'da olmayan bir koşullu tip kontrolü (`if (IS_ERROR...)`
  gibi bir dil düzeyi ifade yok). Yani bir hatayı programatik olarak
  inceleyip devam etme (recover) imkânı henüz yok; `?` sadece ya "hatasızsa
  devam et" ya da "hataysa yukarı fırlat (sonunda uncaught olarak dur)"
  ikiliğini sağlıyor.
- **Tip denetimiyle etkileşim**: `expr?`'in sonucu bir tipli `var`'a
  atanıyorsa (yukarıdaki örnekte `string`), hata olmayan başarı yolunda tip
  denetimi normal çalışır; ama bir hata değeri hiçbir zaman tipli bir
  primitive'in yerine geçmez — `?` olmadan doğrudan tipli bir `var`'a
  atanmaya çalışılırsa `OP_CHECK_TYPE` bunu "expected 'X', got error" diye
  reddeder.

## Native (host) built-in'ler: `fs_*` / `sys_exec`

`io.out` / `io.in` dışında, dosya sistemi ve komut çalıştırma için sabit
adlı (fixed-arity), doğrudan opcode'a giden birkaç native fonksiyon daha var
(bunlar `fn` ile tanımlanan kullanıcı fonksiyonları değil, `len(x)` gibi
derleyicide özel olarak tanınan built-in'ler):

- **`fs_read(path: string): string`** — dosyayı okur, içeriği string olarak
  döner; dosya açılamazsa hata değeri döner (yukarıdaki `?` bölümüne bakın).
  Okunan içerik o an çalışan frame'in region'ına ait (LAM string kuralları
  geçerli — bkz. yukarıdaki LAM bölümü).
- **`fs_write(path: string, content: string): nil`** — dosyaya yazar
  (var olanın üzerine), başarısızsa hata değeri döner, başarılıysa `nil`.
- **`fs_exists(path: string): bool`** — `access()` ile O(1) varlık kontrolü.
- **`sys_exec(cmd: string): string`** — komutu `popen` ile çalıştırır,
  yakalanan stdout'u string olarak döner; komut başlatılamazsa hata değeri
  döner.

Bunlar bilinçli olarak düz (flat), namespace'siz isimler — `fs.read(...)`
gibi noktalı bir erişim yok; bunlar `len(x)` gibi derleyiciye doğrudan
gömülü native'ler, kullanıcı modülü değiller. (`io.` de aynı şekilde bir
kullanıcı modülü değil, derleyiciye gömülü ayrı bir özel durum.) Gerçek
kullanıcı modülleri (`use math;` ile yüklenenler) için noktalı erişim
zaten çalışıyor — bkz. yukarıdaki `use` bölümü.

## Henüz desteklenmiyor (yol haritası)

Bunlar parser tarafından tanınıyor ve net bir "not yet supported" derleme
hatasıyla reddediliyor (crash yok, anlamlı satır numarasıyla hata mesajı var):

- Pointer'lar (`int*`, `&`, `*ptr`), `new` / `delete`
- `sizeof`, `typeof`
- `string` değerlerinde karakter indeksleme (`s[i]`) — parser değil, runtime
  reddediyor ("Cannot index a value of type string")
- Fonksiyon gövdesi **içinde** (local) `struct`/`enum` tanımlamak — üst
  düzeyde (top-level) ikisi de tam çalışıyor, bkz. aşağıda

`struct`, `enum`, `switch`/`case`/`default` ve `defer` artık **üst düzeyde/
fonksiyon içinde çalışıyor** — bkz. yukarıdaki "Şu an çalışan özellikler"
bölümü (bu README'nin önceki bir revizyonu bunları hâlâ yol haritasında
gösteriyordu, güncel değildi). Diziler (`int[]`, `[1, 2, 3]` literal init) ve
map'ler (`{"k": v}`) da desteğe geçti — bkz. yukarıdaki "Diziler (Lists) ve
Map'ler" bölümü. Aynı şekilde `fs_read/fs_write/fs_exists/sys_exec` native
built-in'leri ve hata değerleri (`VAL_ERROR`) + `?` operatörü de artık
çalışıyor — bkz. ilgili bölümler yukarıda.

Kalan maddeler (pointer'lar, `sizeof`/`typeof`, string indeksleme) tip
sistemine ve bellek modeline dokunuyor, bu yüzden kasıtlı olarak bir sonraki
aşamaya bırakıldı.

### Neden `string`/`time`/`path`/`process`/`thread`/`net` stdlib modülleri yok

`math` ve `random` saf Proton ile yazılabildi çünkü ikisi de yalnızca
aritmetik + döngü + fonksiyon istiyor — dilin zaten sahip olduğu şeyler.
`fs` artık bu listede değil: `fs_read/fs_write/fs_exists` doğrudan C
tarafında built-in olarak eklendi (bkz. yukarıdaki "Native (host)
built-in'ler" bölümü) — gerçek bir `stdlib/fs.prt` (saf Proton) hâlâ yok,
ama syscall eksikliği artık engel değil. Kalanlar şu an **imkansız**,
kütüphane eksikliğinden değil, dil çekirdeği eksikliğinden:

- **`string`**: `string` değerlerinde `s[i]` ile karakter indeksleme henüz
  yok (`[]` operatörü şu an yalnızca `ObjList`/`ObjMap` için çalışıyor), o
  yüzden `length`, `substring`, `toUpper` gibi neredeyse hiçbir şey
  yazılamıyor (concatenation ve `len(s)` — bu ikisi zaten çalışıyor —
  dışında).
- **`time`/`process`/`net`**: `sys_exec` ile process/komut çalıştırma artık
  mümkün (yukarıya bkz.), ama `time` (saat okuma) ve `net` (soket) için
  hâlâ hiçbir host/syscall binding'i yok — bunlar saf Proton'da değil, C
  tarafında yeni built-in'ler olarak eklenmesi gerekiyor.
- **`thread`**: VM tek iş parçacıklı (single-threaded), call frame yığını
  paylaşılan global durum üzerine kurulu; gerçek thread desteği VM'in
  kendisine dokunmayı gerektirir.
- **`path`**: teorik olarak string işlemlerine indirgenebilir, yani o da
  `s[i]` string indeksleme desteğini bekliyor.

### Önerilen sıradaki adımlar

Diziler (`ObjList`) ve map'ler (`ObjMap`) artık uygulandı (yukarıdaki
ilgili bölümlere bakın), bu yüzden eski listedeki 1. madde tamamlandı.
Beraberinde getirdiği öncelik olan dizi/list escape analizi de artık
uygulandı (bkz. yukarıdaki "Ömür (lifetime)" notu ve aşağıdaki
"Kalanlar" bölümü) — `promoteEscapingValue` artık `ObjList` için de
çalışıyor, `Stack`/`Queue` gibi collections'ların önündeki blocker
kalktı (collections'ın kendisi hâlâ yazılmadı, bkz. "Kalanlar"):

1. ~~Struct'lar~~ — **tamamlandı.** Üst düzeyde `struct Name { field: type; }`
   ve generic `struct Box<T>` çalışıyor, runtime temsili `ObjMap` üzerinden
   (ayrı bir `ObjInstance` tipi yok). Kalan: `sizeof` hâlâ desteklenmiyor,
   ve local (fonksiyon içi) struct/enum tanımı hâlâ reddediliyor.
2. ~~switch~~ — **tamamlandı.** Beklendiği gibi sözdizimsel şeker olarak,
   yeni bir opcode gerekmeden bytecode'a derleniyor. Not: C tarzı
   fall-through semantiği var (`break` şart), bu tasarım kararı olarak kaldı.
3. **Pointer'lar / `new` / `delete`** — hâlâ yapılmadı. Gerçek bir heap modeli
   ve GC ya da arena allocator kararını gerektiriyor. Struct'lar artık var
   olduğu için (heap nesnesi modeli `ObjMap` üzerinden zaten kurulu), bu artık
   önündeki tek engel değil — asıl karar hâlâ bekleniyor.
4. **String indeksleme (`s[i]`)** — hâlâ yapılmadı; `string`/`path` stdlib
   modüllerinin önündeki tek engel bu; diziler için zaten var olan `[]`
   altyapısı (`OP_GET_INDEX`) örnek alınabilir.
5. Bootstrap hedefi (Proton VM'in Proton ile yazılması) için önce bu C
   interpreter'ın tam özellik setine ulaşması gerekiyor.

### Kalanlar — bu oturumda yapılan işlerin durumu

1. **Modül içi çapraz çağrı düzeltmesi — doğrulandı.** `identifierExpr`'deki
   bare-call yoluna (`compiler.c`) `currentModulePrefix()` ile mangle etme
   eklenmişti; hem bu hem de bare global değişken *okuma* tarafı (aynı
   fonksiyonun `OP_GET_GLOBAL` dalı — örn. `math.prt` içindeki `degToRad`'ın
   `PI`'ı kullanması) rebuild edilip `examples/stdlib_demo.prt` ve
   `examples/stdlib_edge_test.prt` tekrar çalıştırılarak doğrulandı;
   ikisi de beklenen çıktıyı veriyor (edge test'in sonundaki
   `math.sqrt(-1.0)` panic'i de dahil, dosyanın kendisinin beklediği gibi).
2. **Generics uçtan uca test edildi.** `examples/generics_fn_test.prt` ve
   `examples/generics_struct_test.prt` derlenip çalıştırılarak doğru sonuç
   verdiği doğrulandı. Ayrıca ad-hoc edge-case testleri de yapıldı: yanlış
   sayıda type argümanı (`max<int,int>(...)`) derleme zamanında anlamlı bir
   hatayla reddediliyor; iç içe (nested) generic çağrılar
   (`max<int>(max<int>(1,7), max<int>(4,2))`) doğru sonuç veriyor; tip
   uyuşmazlığı (`identity<int>("not a number")`) çalışma zamanında anlamlı
   bir hatayla durduruluyor; bir modül adıyla (`math`) aynı isimde yerel
   değişken tanımlamak, `module.member` erişimini bozmadan güvenle
   çalışıyor (dot-access her zaman modül olarak, bare isim ise yerel
   olarak çözülüyor).
3. **ObjList escape/promotion fix'i tamamlandı.** `vm.c`'deki
   `promoteEscapingValue`, artık `ObjString`'in yanında `ObjList` için de
   çalışıyor: `object.c`'ye eklenen `regionCopyList` (bir üst/çağıranın
   region'ına taşıma, string'lerdeki `regionCopyString` ile birebir aynı
   zincirleme mantıkla) ve `permanentCopyList` (en dıştaki frame'i de
   aşarsa kalıcı/malloc'lu depoya düşme, `freeObjects()`'teki daha önce
   ulaşılamaz olan `OBJ_LIST` dalı da bu yüzden artık gerçek bir işlev
   görüyor). Promotion, listenin elemanlarını da (iç içe listeler ve
   çalışma zamanında üretilen regional string'ler dahil) recursive olarak
   kapsıyor. Test edildi: tek seviye return-escape, return sonrası ağır
   region-churn'e karşı dayanıklılık (`assert` ile doğrulanan, bozulmamış
   veri), regional string içeren listelerin escape'i, ve gerçek iç içe
   liste-içinde-liste escape'i (recursive promotion) — hepsi doğru sonuç
   verdi, çökme yok. Bu, `Stack`/`Queue` gibi collections'ın önündeki
   blocker'ı kaldırıyor; collections'ın kendisi (bu oturumun kapsamı
   dışında) hâlâ yazılmadı.
4. **Test kapsamı hâlâ tam değil.** Yukarıdaki maddelerde belirtilenlerin
   ötesinde sistematik bir test paketi (test suite) yok — testler bu
   oturumda ad-hoc `/tmp` dosyalarıyla elle yazılıp koşuldu,
   `examples/`'a kalıcı bir regresyon testi olarak eklenmedi.

5. **`OP_SET_INDEX`/`OP_BUILD_MAP`/`OP_SET_GLOBAL`/`OP_DEFINE_GLOBAL`
   escape yolu — düzeltildi.** Önceki oturumda not edilen boşluk
   (`OP_SET_INDEX` ile bir üst kapsamdan gelen bir listenin/map'in içine,
   o an çalışan frame'in kendi henüz-promote-edilmemiş regional bir
   değerini yazmak) artık ele alındı: `ObjString`'e `ObjList` ile aynı
   `region` alanı eklendi (`object.h`/`object.c`), ve list-eleman
   promotion'ında kullanılan özel `promoteListElement` fonksiyonu genel
   amaçlı, public `promoteRegionalValue(Region* dest, Value v)`'ye
   dönüştürüldü (bkz. `object.h`). Bu fonksiyon artık dört yazma
   noktasında da çağrılıyor:
   - `OP_SET_INDEX` → map dalı: `promoteRegionalValue(NULL, value)`
     (`ObjMap` her zaman kalıcı/malloc'lu olduğu için hedef her zaman
     permanent storage).
   - `OP_SET_INDEX` → list dalı: `promoteRegionalValue(list->region, value)`
     (hedef, o listenin kendi region'ı — zaten doğru region'daysa
     kopyalama yapılmıyor).
   - `OP_BUILD_MAP` (map literal değerleri) → `promoteRegionalValue(NULL, val)`.
   - `OP_SET_GLOBAL` / `OP_DEFINE_GLOBAL` → `promoteRegionalValue(NULL, value)`
     (`vm.globals` program ömrü boyunca yaşıyor).

   ASan/UBSan altında hem mevcut `examples/` paketi hem de bu üç yazma
   yolunu (liste, map, global) hedefleyen özel escape testleri
   (fonksiyon içinde regional string üretip dışarıdaki bir listeye/
   map'e/global'e 500 kez üst üste yazan döngüler) çalıştırılarak
   doğrulandı — hiçbir use-after-free / heap-buffer-overflow tespit
   edilmedi, sonuçlar tutarlı.

6. **README doğruluk taraması — bu oturumda yapıldı.** README, kodun
   kendisiyle satır satır karşılaştırıldı (`grep`/`sed` ile ilgili
   `compiler.c` bölümleri) ve ad-hoc test dosyalarıyla (`/tmp/*.prt`)
   çalışma zamanında doğrulandı. Sonuç: dokümanın "Henüz desteklenmiyor"
   bölümü güncelliğini yitirmişti — `struct`, `enum`, `switch`/`case`/
   `default` ve `defer` kodda **zaten tam çalışıyordu** ama hâlâ yol
   haritasında listeleniyordu. Bunlar "Şu an çalışan özellikler"e taşındı;
   yol haritasında yalnızca gerçekten desteklenmeyenler kaldı: pointer'lar/
   `new`/`delete`, `sizeof`/`typeof`, string indeksleme (`s[i]`), ve
   fonksiyon içi (local) `struct`/`enum` tanımı. Ayrıca "çalışan
   özellikler"deki int64/uint64 notu, NumKind bölümüyle çelişecek şekilde
   eskiydi (hâlâ "±2^53-1 ile sınırlı" diyordu) — düzeltildi: saklama tam
   64-bit hassasiyette, sadece aritmetik sonrası double'a düşüyor.

7. **Modül sistemi genişletildi — bu oturumda eklendi.** Daha önceki bir
   oturumda zaten gerçek noktalı erişim (`math.sqrt(x)`) eklenmişti, ama
   bu README hâlâ "gerçek bir modül sistemi değil" diyordu (kod ile
   doküman arasında bir tutarsızlık daha) — bu düzeltildi, ayrıca üç
   gerçek eksik giderildi:
   - Var olmayan bir modül (`use mathh;`) artık anlamlı bir derleme
     hatasıyla reddediliyor (`readEntireFile` NULL dönünce artık sessizce
     yutulmuyor); yalnızca `io` (dosyasız, derleyiciye gömülü sözde-
     namespace) bu kuraldan muaf.
   - **`private fn`/`private var`/`private const`** (yeni `TOKEN_PRIVATE`
     keyword'ü) — mangled ismi `privateMemberRegistry`'ye kaydediyor;
     `identifierExpr`'deki `moduleName.member` erişim dalı, çağıran
     modül (`currentModulePrefix()`) hedef modülle aynı değilse
     (self-access değilse) reddediyor. `stdlib/math.prt`'deki iç yardımcı
     `_reduceAngle` artık gerçekten `private` (dışarıdan erişim test
     edilip reddedildiği doğrulandı).
   - **Import alias'ı** (`use math as m;`, yeni `TOKEN_AS` keyword'ü) —
     `moduleRegistry`, düz bir isim listesinden `{alias, prefix}`
     çiftleri listesine dönüştürüldü (`ModuleBinding`), böylece `m.sqrt`
     çağrı yerinde `math.sqrt` mangled anahtarına çözülüyor.
   - Yan etki olarak gerçek bir hata bulundu ve düzeltildi: `use io;`
     yazıldığında `io`, genel modül olarak kaydedilip `io.out`'un
     hardcoded dispatch'ini gölgeliyor ve "Undefined function 'io.out'"
     ile kırılıyordu — `loadStdlibModule` artık `io`'yu asla
     `moduleRegistry`'ye kaydetmiyor.
   - `examples/hello.prt`, `examples/factorial.prt`, `examples/features.prt`
     içindeki anlamsız `use stdlib;` satırları temizlendi (yeni "eksik
     modül → hata" kuralı altında derlenemezlerdi; zaten `io.out` hiçbir
     zaman bir `use` gerektirmiyordu). Tüm `examples/` paketi yeniden
     çalıştırılıp regresyon olmadığı doğrulandı.

## `ml::` Kütüphanesi

Saf Proton ile yazılmış istatistik + lineer cebir + basit ML modülü
(`stdlib/ml.prt`, `use ml;` ile yüklenir). `math::sqrt`'e bağımlı
(modül kendi içinde `use math;` yapıyor, çağıranın ayrıca `use math;`
yazmasına gerek yok).

**Veri temsili:** Vektör → `float64[]`. Matris → `list[]` (her elemanı
bir `float64[]` satır olan bir liste). Proton'un tip anotasyonları tek
seviyeli `T[]`'i destekliyor, `float64[][]` diye bir sözdizimi yok — bu
yüzden matrisler `list[]` (elemanı denetlenmeyen bir dizi) olarak
tanımlanıyor; tekil satırlar (`m[r]`) yine sıradan `float64[]`,
`m[r][c]` gibi normal indekslenebiliyor.

- **Vektör istatistikleri:** `sum`, `mean`, `variance`/`sampleVariance`,
  `stddev`/`sampleStddev`, `vecMin`, `vecMax`, `correlation` (Pearson).
- **Vektör cebiri:** `vecAdd`, `vecSub`, `vecScale`, `dot`, `norm`.
- **Matris cebiri:** `zerosMatrix(rows, cols)`, `matRows`, `matCols`,
  `matAdd`, `matScale`, `matTranspose`, `matMul` (rows x inner çarpı
  inner x cols), `matVecMul`.
- **Aktivasyon fonksiyonları:** `sigmoid`, `relu`, `tanh` (Proton'da
  native `exp()` olmadığı için `tanh`/`sigmoid` sabit-iterasyonlu bir
  Taylor serisi ile yazılmış özel/`private` bir `_exp` yardımcısına
  dayanıyor), ve vektör üzerinde çalışan `sigmoidVec`/`reluVec`.
- **Basit lineer regresyon (tek özellik):** `linRegFit(xs, ys)` kapalı
  form en küçük kareler ile bir `LinRegModel { slope; intercept; }`
  struct'ı döner; `linRegPredict(model, x)`, `linRegRSquared(model, xs, ys)`.
- **Çok değişkenli lineer regresyon:** `gdFit(xs, ys, learningRate, epochs)`
  batch gradient descent ile ağırlık vektörünü (`float64[]`) döner
  (`xs` bir `list[]`, her satır bir örneğin özellik vektörü — bias terimi
  istenirse `xs`'e sabit-1 bir sütun eklenmeli); `gdPredict`, `mse`.
- **K-means kümeleme:** `nearestCentroid(point, centroids)`,
  `kMeansFit(points, initialCentroids, iterations)` — Lloyd algoritmasını
  sabit iterasyon sayısı kadar çalıştırır (dinamik yakınsama kontrolü
  yok, deterministik), bir küme bu turda hiç nokta almazsa 0'a bölme
  yerine eski centroid'i korur.

Örnek:
```
use ml;

fn main() {
    var xs: float64[] = [1.0, 2.0, 3.0, 4.0, 5.0];
    var ys: float64[] = [2.0, 4.0, 6.0, 8.0, 10.0];
    var model: LinRegModel = ml::linRegFit(xs, ys);
    io::out("slope=", model.slope, " intercept=", model.intercept);
    io::out("R^2=", ml::linRegRSquared(model, xs, ys));

    var a: list[] = [[1.0, 2.0], [3.0, 4.0]];
    var b: list[] = [[5.0, 6.0], [7.0, 8.0]];
    var c: list[] = ml::matMul(a, b);
}
```

Tam örnek: `examples/ml_demo.prt`.

## `net::` Kütüphanesi

Senkron, düz-HTTP-üzerinden (TLS yok) bir istemci kütüphanesi, artık
**outbound-only** (yalnızca dışarı bağlanan) bir ham TCP/UDP soket
API'siyle birlikte (aşağıya bakın). Hâlâ kasıtlı olarak **yok**:
`bind`/`listen`/`accept` ya da bunlara benzer *dinleyen* soket
primitifleri -- bunlar port tarayıcı / keyfi arka kapı sunucusu inşa
etmek için gereken temel yapı taşları olduğundan bilinçli olarak kapsam
dışı bırakıldı. `net::serve` bu satırın tek istisnası: dinleme soketini
VM'in kendisi açıp yönetir, script bir soket handle'ı değil yalnızca
ayrıştırılmış HTTP istek/yanıt map'leri görür.

- `net::get(url)` -- GET isteği atar, yanıt gövdesini (`string`) döner,
  hata durumunda `VAL_ERROR`.
- `net::post(url, body)` -- POST isteği atar, yanıt gövdesini döner.
- `net::request(options)` -- esnek istek: `options` bir `map` olup
  `"url"` (zorunlu), `"method"` (varsayılan `"GET"`), `"body"`,
  `"timeout"` (ms, varsayılan 10000, tavan 60000) ve `"headers"`
  (string->string bir alt `map`) anahtarlarını kabul eder. Dönen `map`:
  `{ "status": 200, "body": "...", "headers": {...} }`.
- `net::resolve(hostname)` -- DNS lookup, ilk çözülen IP adresini
  `string` olarak döner. Soket açmaz/bağlanmaz.
- `net::ping(host, timeoutMs)` -- port 80'e tek bir TCP connect
  denemesiyle erişilebilirlik ölçer; başarılıysa geçen süreyi ms
  (`float`) cinsinden, aksi halde `-1` döner. `ECONNREFUSED` bile
  "erişilebilir" sayılır (host cevap verdi demektir).
- `net::urlEncode(str)` / `net::urlDecode(str)` -- percent-encoding,
  hiçbir ağ erişimi yok, saf string dönüşümü.
- `net::serve(port, handler)` -- yerleşik, tek-thread'li blocking bir
  HTTP/1.1 sunucusu. VM, dinleme soketini, accept döngüsünü ve HTTP
  framing'ini (request parse, Content-Length, response yazımı) kendi
  içinde yönetir; script hiçbir zaman ham bir soket handle'ı görmez --
  sadece her istek için `handler`'ı, ayrıştırılmış bir `req` map'i
  (`method`, `path`, `body`, `headers`) ile çağırır ve dönen map'i
  (`status`, `body`) yanıt olarak gönderir. `handler` artık sıradan bir
  **expression**: çıplak bir fonksiyon adı (`net::serve(8080,
  appHandler)`) ya da bir fonksiyon değeri tutan değişken/parametre
  (`net::serve(8080, h)`) olabilir -- Proton artık first-class fonksiyon
  değerlerini destekliyor (aşağıya bakın). Localdev/basit REST API
  senaryoları için uygundur; production-grade concurrent bir sunucu
  değildir.

### Outbound-only soket API'si: `net::connect` / `send` / `recv` / `close`

Genel amaçlı bir istemci/protokol katmanı yazmak için (custom DB
sürücüsü, IoT protokolü, P2P client tarafı, vb.) script artık dışarı
bağlanan bir TCP/UDP soketi açıp ham byte akışı üzerinde
send/recv yapabilir. Script'e verilen değer küçük bir tamsayı
**handle**'dır (VM'in dahili `netSockets` tablosuna bir index), gerçek
bir dosya tanımlayıcısı (fd) değil.

- `net::connect(host, port, protocol)` -- `protocol` `"tcp"` ya da
  `"udp"` olmalı. Başarılıysa bir `handle` (`number`) döner, aksi halde
  `VAL_ERROR`. Bağlantı/gönderim/alım işlemleri 15 saniyelik bir
  timeout'a tabidir (donmuş/erişilemeyen bir host tüm interpreter'ı
  sonsuza dek kilitleyemesin diye).
- `net::send(handle, data)` -- `data` (`string`) gönderir, gönderilen
  byte sayısını (`number`) döner, hata durumunda `VAL_ERROR`.
- `net::recv(handle, maxBytes)` -- en fazla `maxBytes` (1 ile 1 MiB
  arasına clamp'lenir) byte okur, `string` olarak döner (karşı taraf
  bağlantıyı düzgünce kapattıysa boş string), hata durumunda
  `VAL_ERROR`.
- `net::close(handle)` -- soketi kapatır, `nil` döner. Geçersiz ya da
  zaten kapalı bir handle sessiz bir no-op'tur (hata fırlatmaz).

**Kasıtlı olarak yok:** `bind`, `listen`, `accept` -- script bu API ile
asla dinleyen bir soket açamaz ya da gelen bir bağlantı kabul edemez;
yalnızca dışarı bağlanabilir. Açık soket sayısı sabit ve küçük bir
tabloyla (`NET_SOCKETS_MAX = 64`) sınırlıdır.

Örnek (TCP client):
```
fn main() {
    var h: int = net::connect("example.com", 80, "tcp");
    net::send(h, "GET / HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n");
    var resp: string = net::recv(h, 4096);
    io::out(resp);
    net::close(h);
}
```

Örnek sunucu (handler değişkende tutulabilir):
```
fn appHandler(req: map) {
    return {
        "status": 200,
        "body": "{\"message\": \"Proton 6 HTTP Server Calisiyor!\"}"
    };
}

fn main() {
    var h: fn = appHandler;
    net::serve(8080, h);
}
```

Örnek:
```
var opts: map = { "method": "GET", "url": "http://example.com/", "timeout": 5000 };
var res: map = net::request(opts);
io::out(res["status"], " ", len(res["body"]));
```

## First-class fonksiyon değerleri

Fonksiyonlar artık sıradan `Value`'lar gibi taşınabilir: bir değişkene
atanabilir, bir başka fonksiyona parametre olarak geçilebilir (örn.
`net::serve`'in `handler` argümanı), yeniden atanabilir. Bir parametre
ya da local'in "bir fonksiyon değeri tutacağını" belirtmek için `fn`
tip adı olarak kullanılabilir (unchecked, ama sözdizimsel niyeti
belirtir):

```
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
    io::out(g(4));                // 64
}
```

**Kasıtlı olarak yok: closure / upvalue capture.** Bir fonksiyon değeri
yalnızca kod + isme işaret eder; tanımlandığı kapsamdaki hiçbir local
değişkeni yakalamaz/taşımaz (state'siz first-class fonksiyonlar).
Gerçek closure desteği, mevcut region-tabanlı bellek modeliyle
(her call frame'in kendi region'ını dönüşte yok etmesi) ciddi bir
tasarım gerilimi yaratacağından bilinçli olarak bu aşamada eklenmedi.

## Bitwise operatörler

`&` (AND), `|` (OR), `^` (XOR), `~` (unary NOT), `<<` (sol shift), `>>`
(sağ shift). Operandlar 64-bit işaretli tamsayıya (`int64_t`) truncate
edilir (C'nin double->int dönüşümüyle aynı davranış), sonuç `NUM_I64`
kind'inde bir sayı olarak döner. Öncelik sırası C'ninkine benzer:
`|` < `^` < `&` < eşitlik/karşılaştırma < `<<`/`>>` < `+`/`-` < `*`/`/`/`%`.
Shift miktarı 0-63 aralığı dışındaysa runtime hatası verir.

```
var a: int = 12;
var b: int = 10;
io::out(a & b);   // 8
io::out(a | b);   // 14
io::out(a ^ b);   // 6
io::out(~a);      // -13
io::out(a << 2);  // 48
io::out(a >> 2);  // 3
```
