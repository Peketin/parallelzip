Parallel Zip

Tassa osassa toteutetaan rinnakkainen versio aiemmasta RLE pakkausohjelmasta. Ohjelman nimi on pzip. Lisaksi mukana on rinnakkainen purkuohjelma punzip.

Tarkoitus
Tehtavan tarkoituksena on harjoitella POSIX saikeiden kayttoa Linux ymparistossa. 
Tavoitteena on oppia, miten ohjelman tyota voidaan jakaa usealle saikeelle ja miten rinnakkaisuus voidaan toteuttaa niin, etta lopputulos pysyy oikeassa jarjestyksessa.

Ohjelmat
pzip pakkaa yhden tai useamman tiedoston kayttaen run length encoding menetelmaa.
punzip purkaa pzip ohjelman tai aiemman my zip ohjelman tuottamaa RLE muotoista pakattua dataa.

Pakkausformaatti
Pakkausformaatti on sama kuin aiemmassa my zip tehtavassa. Jokainen pakattu jakso koostuu neljan tavun kokonaisluvusta ja yhdesta merkista.
Kokonaisluku kertoo, kuinka monta kertaa merkki toistuu. Merkki kertoo, mika merkki toistuu.
Kyseessa ei ole oikea zip arkisto. Tiedostopaate voi olla esimerkiksi z, mutta formaatti on tehtavan oma yksinkertainen RLE formaatti.

Rinnakkaisuus
pzip jakaa syotteen pienempiin osiin. Tyontekijasaikeet pakkaavat naita osia rinnakkain. Jokainen saie hakee seuraavan vapaan tyon yhteisesta tyolistasta.
Yhteisen tyoindeksin kaytto suojataan mutex lukolla. Talla varmistetaan, etta kaksi saietta ei ota samaa tyota kasiteltavaksi.
Varsinainen pakkaus tehdään ilman jatkuvaa lukitusta, koska jokainen saie kirjoittaa oman tuloksensa omaan muistialueeseensa. Tama vahentaa lukituksen aiheuttamaa hidastusta.

Tulosten jarjestys
Vaikka pakkaus tehdään rinnakkain, lopullinen tulostus tehdään oikeassa alkuperaisessa jarjestyksessa. 
Tama on tarkeaa, koska pakattu data taytyy vastata alkuperaista syotevirtaa.
Lisaksi viereiset RLE jaksot yhdistetaan, jos edellisen palan viimeinen merkki ja seuraavan palan ensimmainen merkki ovat samat. 
Nain pakkaus toimii oikein myos palojen ja tiedostorajojen yli.

Usean tiedoston pakkaus
pzip tukee useita syötetiedostoja samalla komentorivilla. Useat tiedostot kasitellaan yhtena jatkuvana syotevirtana.
Tama tarkoittaa, etta jos ensimmaisen tiedoston lopussa ja seuraavan tiedoston alussa on sama merkki, RLE jaksot yhdistetaan oikein.
Usean tiedoston purku
punzip tukee useita pakattuja syötetiedostoja samalla komentorivilla. Tiedostot puretaan annetussa jarjestyksessa ja tulos kirjoitetaan standard outputiin.

Saikeiden maara
Ohjelma valitsee saikeiden maaran kaytettavissa olevien prosessoriytimien perusteella. 
Toteutus on tarkoitettu Linux ymparistoon ja kayttaa GNU Linux ympariston tarjoamaa get nprocs toimintoa.
Taman vuoksi lahdekoodin alussa kaytetaan GNU SOURCE maaritysta. Se on tassa tehtavassa hyvaksyttava ratkaisu, koska tehtava on Linux pohjainen.

Kaantaminen
Ohjelmat kaannetaan gcc kaantajalla pthread tuella. Pthread tuki tarvitaan, koska ohjelma kayttaa POSIX saikeita.

Testaus
Oikeellisuus testataan pakkaamalla tiedosto, purkamalla se takaisin ja vertaamalla purettua tulosta alkuperaiseen tiedostoon.
Jos vertailukomento ei tulosta mitaan, tiedostot ovat samat ja ohjelma toimii oikein.

Dokumentointi
Lahdekoodissa on kommentteja jokaisen tarkean osan valissa. Kommentit selittavat esimerkiksi tietorakenteet, 
tyojonon, mutexin kayton, saikeiden tehtavan, RLE logiikan, tulosten yhdistamisen ja resurssien vapauttamisen.

Yhteenveto
Parallel Zip osuudessa on toteutettu rinnakkainen RLE pakkaus ja rinnakkainen purku. Toteutus kayttaa POSIX saikeita, 
tukee useita tiedostoja, sailyttaa tulosten oikean jarjestyksen ja kasittelee RLE jaksojen yhdistamisen oikein palojen ja tiedostorajojen yli.
