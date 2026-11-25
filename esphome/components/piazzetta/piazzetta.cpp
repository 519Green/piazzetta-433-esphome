#include "piazzetta.h"

#include <cmath>

#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>
#include "esp_timer.h"
#include "soc/gpio_reg.h"

#include "esphome/core/log.h"

namespace esphome {
namespace piazzetta {

static const char *const TAG = "piazzetta";

// --- Protocole (voir telecommande-poele-433-protocole.md) ---
static const float UNITE_US = 416.67f;          // 2 400 bauds
static const uint8_t ETAT_REPOS = 0x3D, ETAT_MENU = 0xBD;
static const uint8_t TOUCHE_PLUS = 0x88, TOUCHE_MOINS = 0x8C;
static const uint8_t TOUCHE_VENTILATEUR = 0x84, TOUCHE_PUISSANCE = 0x98;
static const uint8_t TOUCHE_MARCHE = 0x81;
// SET ROOM TEMPERAT. (consigne) : 0x90 hors trappe ; avec la trappe, la meme touche est ESC (2026-11-20)
static const uint8_t TOUCHE_CONSIGNE = 0x90;
// Trappe ouverte (3e octet 0x80) : MENU 84, + 88, - 8C, SET 98, ESC 90 (capture du 2026-10-31)
static const uint8_t PORTE_MENU = 0x84, PORTE_PLUS = 0x88, PORTE_MOINS = 0x8C, PORTE_SET = 0x98, PORTE_ESC = 0x90;
static const int ENS_APPUIS_LISTE_MAX = 24;
// Interrogation (00 sur F0203-1480V3, 80 sur MULTICOMFORT V5) et maintien de ⏻ (61 / A1) : options YAML
static const uint8_t MARQUE_VENTILATEUR = 0x00, MARQUE_PUISSANCE = 0x50;

// --- Reception ---
static const uint32_t FIN_TRAME_US = 40000;     // le poele marque une pause au milieu de ses ecrans
static const uint32_t TRAME_MAX_US = 1500000;
static const size_t MAX_DUREES = 1600;
static const int MIN_IMPULSIONS = 8;

// --- Taches ---
static const uint32_t DELAI_REPONSE_MS = 1500;
static const int ESSAIS_MAX = 4;
static const int PAS_MAX = 8;
// Energy Saving : pas de marche dans le menu si un ecran (trame B5 de 20 octets ou plus) a ete rejete depuis moins
// de 3 min. Le poele brouille ses propres trames pendant les transitions (fin d'allumage 16:57-17:04 et passage
// en pause 14:43 le 2026-11-24 : impulsions parasites de 64 a 256 us, signal fort) ; les 4 renvois rapides
// tombaient tous dans le brouillage. HA (keep-settings) redemande plus tard.
static const uint32_t CALME_ENS_MS = 180000;
static const size_t OCTETS_ECRAN_MIN = 20;
// Appui long sur ⏻ calque sur la vraie telecommande : 81, puis 11 trames 61 « bouton maintenu »
// avec cet octet d'etat. Valide depuis le banc : allumage lance par l'ESP.
static const uint8_t ETATS_MAINTIEN[] = {0xBE, 0xBE, 0xBE, 0xBE, 0x3E, 0x3E, 0x3E, 0x3E, 0xBE, 0xBE, 0x3E};
static const int APPUIS_MAINTIEN = sizeof(ETATS_MAINTIEN);
static const uint32_t SILENCE_TELECOMMANDE_MS = 10000;
static const uint32_t DELAI_JOIGNABLE_MS = 120000;
// Le poele n'envoie l'ecran complet (B5) que si son contenu a change ou apres une touche ;
// sinon il repond l'heure seule (B9). Au demarrage, si aucun ecran principal n'arrive,
// on ouvre le menu ventilateur sans rien regler : il se referme seul, et l'interrogation
// suivante ramene l'ecran principal complet (constate sur le poele de reference).
static const uint32_t DELAI_AVANT_SYNCHRO_MS = 20000;
static const uint32_t DUREE_SYNCHRO_MS = 12000;
// Meme synchronisation en cours de route : une trame d'ecran manquee laissait HA fige sur une
// phase perimee. Ecran principal pas revu depuis 5 min en phase de transition,
// 30 min en marche ou a l'arret, ou 45 s apres une marche/arret.
static const uint32_t RESYNCHRO_TRANSITION_MS = 5 * 60000;
static const uint32_t RESYNCHRO_STABLE_MS = 30 * 60000;
static const uint32_t RESYNCHRO_APRES_MARCHE_ARRET_MS = 45000;
// Anti-rebond : l'etat ne change qu'apres plusieurs secondes, et chaque appui long bascule.
// Trois appuis en 47 s ont eteint puis relance le poele de reference.
static const uint32_t ANTI_REBOND_MARCHE_ARRET_MS = 90000;
// Verrou jusqu'a confirmation : apres une bascule, aucune autre tant qu'un ecran principal recu
// depuis n'a pas montre un Etat different de celui d'avant (plafond 20 min). Sans lui, un ecran
// fige faisait renvoyer la bascule par le thermostat toutes les 5 min (constate sur le poele de reference).
// En attendant, resynchronisation toutes les 2 min pour obtenir la confirmation.
static const uint32_t VERROU_BASCULE_MAX_MS = 20 * 60000;
static const uint32_t RESYNCHRO_BASCULE_MS = 2 * 60000;
// Diagnostics publies dans HA (age de l'ecran, compteurs de trames)
static const uint32_t PERIODE_DIAGNOSTIC_MS = 60000;
// Fusion des parasites : segments plus courts que 0,45 unite (187 us). La sortie DIO2 est decoupee
// en pas de 64 us : un parasite de 2 pas (128 us) passait l'ancien seuil de 0,3 unite (125 us).
// Valide sur plus de 1 000 trames enregistrees (aucune regression) et par injection de parasites :
// parasites de 128 us recuperes a 99 % (contre 11 %), aucune trame fausse acceptee.
static const float SEUIL_FUSION = 0.45f;

// Fronts horodates par l'interruption (tampon circulaire)
static const uint32_t TAILLE_FRONTS = 4096;
static volatile uint32_t s_t[TAILLE_FRONTS];
static volatile uint8_t s_niveau[TAILLE_FRONTS];
static volatile uint32_t s_tete = 0;
static uint32_t s_queue = 0;
static int s_pin = 32;

static SPIClass *s_spi = nullptr;
static Module *s_module = nullptr;
static SX1278 *s_radio = nullptr;

static void IRAM_ATTR isr_front() {
  uint32_t t = (uint32_t) esp_timer_get_time();
  uint32_t niveau = s_pin < 32 ? (REG_READ(GPIO_IN_REG) >> s_pin) & 1 : (REG_READ(GPIO_IN1_REG) >> (s_pin - 32)) & 1;
  uint32_t i = s_tete & (TAILLE_FRONTS - 1);
  s_t[i] = t;
  s_niveau[i] = niveau;
  s_tete = s_tete + 1;
}

static uint32_t maintenant_us() { return (uint32_t) esp_timer_get_time(); }

static void verifier(int16_t etat, const char *etape) {
  if (etat != RADIOLIB_ERR_NONE)
    ESP_LOGW(TAG, "Radio : %s a echoue (code %d)", etape, etat);
}

static uint8_t somme(const std::vector<uint8_t> &o, size_t n) {
  uint8_t s = 0;
  for (size_t i = 0; i < n; i++)
    s += o[i];
  return s;
}

static std::string hexa(const std::vector<uint8_t> &o) {
  std::string s;
  char b[4];
  for (auto v : o) {
    snprintf(b, sizeof(b), "%02X ", v);
    s += b;
  }
  return s;
}

static std::string rogner(const std::string &s) {
  size_t a = s.find_first_not_of(' ');
  if (a == std::string::npos)
    return "";
  size_t b = s.find_last_not_of(' ');
  std::string r = s.substr(a, b - a + 1);
  // espaces multiples -> un seul
  std::string out;
  for (char c : r)
    if (!(c == ' ' && !out.empty() && out.back() == ' '))
      out += c;
  return out;
}

// Caractere de l'afficheur : chiffres envoyes bruts (0-9), bit 7 = attribut, 5E = degre
static std::string car_lcd(uint8_t v) {
  uint8_t c = v & 0x7F;
  if (c <= 9)
    return std::string(1, char('0' + c));
  if (c == 0x5E)
    return "\xC2\xB0";
  if (c >= 32 && c < 127)
    return std::string(1, char(c));
  return "?";
}

static bool est_chiffre(uint8_t v) { return (v & 0x7F) <= 9; }

// Grandes categories pour l'historique HA (phases observees ; libelle exact du defaut a confirmer)
static const char *const ETAT_DEFAUT = "Fault";
static const char *const ETAT_EXTINCTION = "Shutting down";
static const char *const ETAT_ARRET = "Off";
static const char *const ETAT_ALLUMAGE = "Ignition";
static const char *const ETAT_MARCHE = "Running";
static const char *const ETAT_NETTOYAGE = "Cleaning";
static const char *const ETAT_AUTRE = "Other";
// Arret ENERGY SAVING (« ENERGY SAVING OK ») : en attente de rallumage automatique (2026-11-23)
static const char *const ETAT_PAUSE = "Paused";

static std::string etat_de(const std::string &phase) {
  auto a = [&](const char *mot) { return phase.find(mot) != std::string::npos; };
  // Avertissements de securite « E1 » a « E19 » (notice FWDA00) : un mot qui commence par E suivi d'un chiffre
  bool code_e = false;
  for (size_t i = 0; i + 1 < phase.size(); i++)
    if (phase[i] == 'E' && (i == 0 || phase[i - 1] == ' ') && phase[i + 1] >= '0' && phase[i + 1] <= '9')
      code_e = true;
  // Libelles HA en anglais (2026-11-19). Textes du panneau reconnus en francais et en anglais (P963 C, FWDA00) :
  // « NO LIGHTING » = echec d'allumage, « CONTROL », « START PHASE I/II », « LEVEL / ENS P1 », « GRATE CLEANING »,
  // et d'apres la notice P163 : « SHUT DOWN », « WAIT FOR OFF » (contient OFF : teste avant « OFF »).
  if (code_e || a("DEFAUT") || a("ALARM") || a("ERREUR") || a("ERR") || a("NO LIGHTING"))
    return ETAT_DEFAUT;
  if (a("EXTINCTION") || a("REFROID") || a("SHUT DOWN") || a("WAIT FOR OFF"))
    return ETAT_EXTINCTION;
  if (a("OFF"))
    return ETAT_ARRET;
  if (a("CONTROLE") || a("ALLUMAGE") || a("CONTROL") || a("START PHASE"))
    return ETAT_ALLUMAGE;
  if (a("NIVEAU") || a("LEVEL"))
    return ETAT_MARCHE;
  if (a("NETTOYAGE") || a("CLEANING"))
    return ETAT_NETTOYAGE;
  if (a("ENERGY SAVING"))
    return ETAT_PAUSE;
  return ETAT_AUTRE;
}

// Defauts d'un octet recu
static const uint8_t PARITE_FAUSSE = 1, STOP_FAUX = 2;

// Bits NRZ (1 unite = 1 bit) -> octets : liaison serie inversee, 8 bits poids faible d'abord, parite paire.
// `faux` recoit, pour chaque octet, ses defauts (PARITE_FAUSSE, STOP_FAUX, 0 si l'octet est juste).
static std::vector<uint8_t> octets_de(const std::vector<uint32_t> &durees, std::vector<uint8_t> &faux) {
  faux.clear();
  // Fusion des parasites, le plus court d'abord : un segment trop court (impulsion dans un silence,
  // ou trou dans une impulsion) est fusionne avec ses deux voisins, ce qui garde l'alternance.
  std::vector<uint32_t> d(durees.begin(), durees.end());
  const uint32_t seuil = (uint32_t) (SEUIL_FUSION * UNITE_US);
  while (d.size() >= 3) {
    size_t k = 0;
    for (size_t i = 1; i + 1 < d.size(); i++)
      if (d[i] < seuil && (k == 0 || d[i] < d[k]))
        k = i;
    if (k == 0)
      break;
    d[k - 1] += d[k] + d[k + 1];
    d.erase(d.begin() + k, d.begin() + k + 2);
  }
  std::string bits;
  for (size_t i = 0; i < d.size(); i++) {
    int n = std::max(1, (int) lroundf(d[i] / UNITE_US));
    if (i % 2)
      n = std::min(n, 40);
    bits.append(n, (i % 2) ? '0' : '1');
  }
  // Synchro = 12 zeros ou plus, sauf une fausse synchro suivie du preambule
  size_t debut = std::string::npos, pos = 0;
  while ((pos = bits.find("000000000000", pos)) != std::string::npos) {
    size_t fin = bits.find('1', pos);
    if (fin == std::string::npos)
      break;
    if (bits.compare(fin, 6, "101010") != 0 || bits.find("000000000000", fin) > fin + 16) {
      debut = fin;
      break;
    }
    pos = fin;
  }
  std::vector<uint8_t> out;
  if (debut == std::string::npos)
    return out;
  bits.append(12, '0');
  size_t i = debut;
  while (i + 11 <= bits.size()) {
    if (bits[i] != '1') {
      i++;
      continue;
    }
    uint8_t v = 0;
    int uns = 0;
    for (int k = 0; k < 8; k++)
      if (bits[i + 1 + k] == '1') {
        v |= 1 << k;
        uns++;
      }
    if (bits[i + 9] == '1')
      uns++;
    faux.push_back((uns % 2 != 0 ? PARITE_FAUSSE : 0) | (bits[i + 10] != '0' ? STOP_FAUX : 0));
    out.push_back(v);
    i += 11;
  }
  // Zeros de fin = silence, mais jamais dans la longueur utile : une somme peut valoir 00. L'ecran
  // ENERGY SAVING STOP 6F (B5 20 06 46 ... 40 C1 18 00) perdait ainsi sa somme et etait toujours rejete :
  // toutes les marches vers +6 du 2026-11-24 butaient sur ce pas (capture 18:20).
  const size_t utile = out.empty() ? 0 : out[0] == 0xB5 ? 32 : out[0] == 0xB9 ? 6 : 5;
  while (out.size() > std::max<size_t>(utile, 1) && out.back() == 0) {
    out.pop_back();
    faux.pop_back();
  }
  return out;
}

// Vrai si les n premiers octets ont une parite et un bit de stop justes. La somme sur 8 bits seule
// a laisse passer des ecrans corrompus (« NIVEA? P1 » a 9 °C, texte illisible a 186 °C).
static bool octets_justes(const std::vector<uint8_t> &faux, size_t n) {
  for (size_t i = 0; i < n && i < faux.size(); i++)
    if (faux[i] != 0)
      return false;
  return true;
}

// Defaut le plus frequent avec une alimentation bruitee : une impulsion parasite pendant un silence
// ajoute UN bit a 1 dans un octet (08 -> 48, 20 -> 50, constate sur le poele de reference).
// On le retire si les deux controles designent le meme bit : un seul octet faux parmi les n utiles,
// par sa parite seule, et un ecart de somme egal a un bit a 1 de cet octet. Sinon on ne touche a rien.
static bool corriger_bit_parasite(std::vector<uint8_t> &o, std::vector<uint8_t> &faux, size_t n) {
  if (o.size() < n || faux.size() < n)
    return false;
  size_t k = n;
  for (size_t i = 0; i < n; i++) {
    if (faux[i] == 0)
      continue;
    if (faux[i] != PARITE_FAUSSE || k != n)
      return false;
    k = i;
  }
  if (k == n || k == 0)  // rien a corriger, ou l'octet de type lui-meme
    return false;
  uint8_t s = somme(o, n - 1);
  // octet de donnees : la somme calculee depasse la somme recue du bit parasite ;
  // octet de somme : c'est la somme recue qui depasse
  uint8_t ecart = k < n - 1 ? (uint8_t) (s - o[n - 1]) : (uint8_t) (o[n - 1] - s);
  if (ecart == 0 || (ecart & (ecart - 1)) != 0 || (o[k] & ecart) == 0)
    return false;
  o[k] &= ~ecart;
  faux[k] = 0;
  return true;
}

// ---------------------------------------------------------------- Entites

void PiazzettaNumber::control(float value) {
  if (this->parent_ != nullptr)
    this->parent_->demander_reglage(this->reglage_, (int) lroundf(value));
}

void PiazzettaButton::press_action() {
  if (this->parent_ != nullptr)
    this->parent_->demander_action(this->action_);
}

void PiazzettaSwitch::write_state(bool state) {
  if (this->parent_ != nullptr)
    this->parent_->regler_option(this->option_, state);
}

void PiazzettaComponent::regler_option(Option option, bool actif) {
  if (option == Option::EMISSION) {
    // Couper l'emission en cours de tache (appui long compris) sert d'arret d'urgence : les trames suivantes sont bloquees
    this->ecoute_seule_ = !actif;
    ESP_LOGW(TAG, "Emission %s", actif ? "AUTORISEE" : "bloquee (ecoute seule)");
    if (this->emission_ != nullptr)
      this->emission_->publish_state(actif);
  } else {
    this->synchro_auto_ = actif;
    ESP_LOGW(TAG, "Synchronisation automatique %s", actif ? "activee" : "desactivee");
    if (this->synchro_auto_sw_ != nullptr)
      this->synchro_auto_sw_->publish_state(actif);
  }
}

// ---------------------------------------------------------------- Radio

void PiazzettaComponent::configurer_radio_() {
  verifier(s_radio->beginFSK(), "beginFSK");
  verifier(s_radio->setFrequency(this->frequence_), "setFrequency");
  verifier(s_radio->setOOK(true), "setOOK");
  verifier(s_radio->setCrcFiltering(false), "setCrcFiltering");
  // Reglages repris de rtl_433_ESP, valides sur ce montage
  verifier(s_radio->setDataShapingOOK(2), "setDataShapingOOK");
  verifier(s_radio->setOokThresholdType(RADIOLIB_SX127X_OOK_THRESH_PEAK), "setOokThresholdType");
  verifier(s_radio->setOokPeakThresholdDecrement(RADIOLIB_SX127X_OOK_PEAK_THRESH_DEC_1_1_CHIP), "setOokPeakThresholdDecrement");
  verifier(s_radio->setOokPeakThresholdStep(RADIOLIB_SX127X_OOK_PEAK_THRESH_STEP_0_5_DB), "setOokPeakThresholdStep");
  verifier(s_radio->setOokFixedOrFloorThreshold(0x50), "setOokFixedOrFloorThreshold");
  verifier(s_radio->setBitRate(1.2), "setBitRate");
  verifier(s_radio->setRxBandwidth(250), "setRxBandwidth");
  verifier(s_radio->setRSSIConfig(RADIOLIB_SX127X_RSSI_SMOOTHING_SAMPLES_2, RADIOLIB_SX127X_OOK_AVERAGE_OFFSET_0_DB), "setRSSIConfig");
  verifier(s_module->SPIsetRegValue(RADIOLIB_SX127X_REG_PREAMBLE_DETECT, RADIOLIB_SX127X_PREAMBLE_DETECTOR_OFF), "preambleDetectOff");
  verifier(s_radio->setDirectSyncWord(0, 0), "setDirectSyncWord");
  verifier(s_radio->disableBitSync(), "disableBitSync");
  verifier(s_radio->setOutputPower(this->puissance_emission_), "setOutputPower");
}

void PiazzettaComponent::ecouter_() {
  pinMode(this->dio2_, INPUT);
  verifier(s_radio->receiveDirect(), "receiveDirect");
  this->en_trame_ = false;
  s_queue = s_tete;
  attachInterrupt(digitalPinToInterrupt(this->dio2_), isr_front, CHANGE);
}

void PiazzettaComponent::emettre_octets_(std::vector<uint8_t> octets) {
  octets.push_back(somme(octets, octets.size()));
  // Preambule 1010101, synchro de ~21 bits, puis les octets (start 1, 8 bits LSB d'abord, parite paire, stop 0)
  std::string bits = "1010101" + std::string(21, '0');
  for (uint8_t v : octets) {
    int uns = 0;
    bits += '1';
    for (int k = 0; k < 8; k++) {
      bool b = (v >> k) & 1;
      bits += b ? '1' : '0';
      uns += b;
    }
    bits += (uns % 2) ? '1' : '0';
    bits += '0';
  }
  while (!bits.empty() && bits.back() == '0')
    bits.pop_back();
  std::vector<uint32_t> d;
  for (size_t i = 0; i < bits.size();) {
    size_t j = i;
    while (j < bits.size() && bits[j] == bits[i])
      j++;
    d.push_back((uint32_t) lroundf((j - i) * UNITE_US));
    i = j;
  }
  if (this->ecoute_seule_) {
    ESP_LOGD(TAG, "Emission bloquee (ecoute seule) : %s", hexa(octets).c_str());
    return;
  }
  ESP_LOGD(TAG, "Emission : %s", hexa(octets).c_str());

  detachInterrupt(digitalPinToInterrupt(this->dio2_));
  this->en_trame_ = false;
  // En emission continue, le SX127x echantillonne DIO2 au rythme du debit binaire :
  // debit OOK maximal et sans mise en forme, sinon la trame est deformee (constate sur le poele de reference).
  verifier(s_radio->setBitRate(32.768), "setBitRate TX");
  verifier(s_radio->setDataShapingOOK(0), "setDataShapingOOK TX");
  verifier(s_radio->transmitDirect(), "transmitDirect");
  pinMode(this->dio2_, OUTPUT);
  digitalWrite(this->dio2_, LOW);
  delay(2);
  int64_t cible = esp_timer_get_time();
  for (size_t i = 0; i < d.size(); i++) {
    digitalWrite(this->dio2_, (i % 2 == 0) ? HIGH : LOW);
    cible += d[i];
    while (esp_timer_get_time() < cible) {
    }
  }
  digitalWrite(this->dio2_, LOW);
  verifier(s_radio->standby(), "standby");
  // Apres une emission, le recepteur peut rester desensibilise : radio entierement reconfiguree
  this->configurer_radio_();
  this->ecouter_();
}

void PiazzettaComponent::finir_trame_() {
  this->en_trame_ = false;
  this->derniere_reception_ms_ = millis();  // pour le delai avant la touche suivante
  if (this->nb_impulsions_ < MIN_IMPULSIONS || this->rssi_max_ < this->seuil_rssi_ || this->durees_.size() < 2)
    return;
  if (this->durees_.size() % 2 == 0)
    this->durees_.pop_back();  // finir sur un « haut »
  this->traiter_trame_(this->durees_, (int) this->rssi_max_);
}

void PiazzettaComponent::traiter_fronts_() {
  while (s_queue != s_tete) {
    if (s_tete - s_queue > TAILLE_FRONTS) {
      s_queue = s_tete - TAILLE_FRONTS;
      ESP_LOGW(TAG, "Tampon de fronts plein (bruit radio continu ?)");
    }
    uint32_t i = s_queue & (TAILLE_FRONTS - 1);
    uint32_t t = s_t[i];
    uint8_t niveau = s_niveau[i];
    s_queue++;

    if (!this->en_trame_) {
      if (niveau != 1)
        continue;
      // Le comparateur OOK bavarde dans le bruit : on ne demarre que sur un signal assez fort
      float r = s_radio->getRSSI(false, true);
      if (r < this->seuil_rssi_)
        continue;
      this->en_trame_ = true;
      this->debut_trame_ = t;
      this->dernier_front_ = t;
      this->dernier_niveau_ = 1;
      this->durees_.clear();
      this->nb_impulsions_ = 1;
      this->rssi_max_ = r;
      this->dernier_rssi_us_ = maintenant_us();
      this->dernier_fort_ = this->dernier_rssi_us_;
      continue;
    }
    if (niveau == this->dernier_niveau_)
      continue;
    if (this->durees_.size() < MAX_DUREES)
      this->durees_.push_back(t - this->dernier_front_);
    if (niveau == 1)
      this->nb_impulsions_++;
    this->dernier_front_ = t;
    this->dernier_niveau_ = niveau;
    if (this->durees_.size() >= MAX_DUREES || t - this->debut_trame_ > TRAME_MAX_US)
      this->finir_trame_();
  }
}

// ---------------------------------------------------------------- Decodage

void PiazzettaComponent::traiter_trame_(const std::vector<uint32_t> &durees, int rssi) {
  std::vector<uint8_t> faux;
  std::vector<uint8_t> o = octets_de(durees, faux);
  if (o.size() < 3)
    return;
  // Longueur utile selon le type de trame (ecran 32, heure 6, telecommande 5) : les octets
  // parasites apres la somme ne comptent pas
  size_t utile = o[0] == 0xB5 ? 32 : o[0] == 0xB9 ? 6 : 5;
  bool corrigee = (o[0] == 0xB5 || o[0] == 0xB9 || o[0] == this->remote_id_) && corriger_bit_parasite(o, faux, utile);
  bool somme_ok = somme(o, o.size() - 1) == o.back();
  bool octets_ok = octets_justes(faux, utile);
  ESP_LOGD(TAG, "Trame %d dBm : %s%s%s%s", rssi, hexa(o).c_str(), somme_ok ? "" : "(somme fausse)",
           corrigee ? " (bit parasite corrige)" : "", octets_ok ? "" : " (octet faux, rejetee)");
  // Compteurs de diagnostic : une trame d'au moins 5 octets non acceptee ci-dessous est rejetee
  if (o.size() >= 5)
    this->nb_trames_rejetees_++;
  // Ecran rejete tant qu'il n'est pas accepte plus bas (on remet alors l'ancienne valeur). Seulement les trames
  // qui commencent par B5 : une collision telecommande + reponse (C5 3B 00 80 80 AA B5 ...) n'est pas du brouillage.
  const uint32_t ecran_rejete_avant = this->dernier_ecran_rejete_ms_;
  if (o.size() >= OCTETS_ECRAN_MIN && o[0] == 0xB5)
    this->dernier_ecran_rejete_ms_ = std::max<uint32_t>(millis(), 1);
  if (!octets_ok)
    return;
  // Derniere trame valide (telecommande ou poele), pour suivre les codes depuis HA ou l'ecran
  if (this->derniere_trame_ != nullptr && somme_ok) {
    std::string t = hexa(o);
    if (!t.empty())
      t.pop_back();
    this->derniere_trame_->publish_state("[" + std::to_string(rssi) + "] " + t);
  }

  // Trame de telecommande (5 octets, somme juste, 2e octet 3D/BD/3E/BE) avec un autre identifiant :
  // on l'annonce, pour qu'un nouvel utilisateur sache quelle valeur mettre dans remote_id.
  if (o.size() >= 5 && somme(o, 4) == o[4] && (o[1] & 0x7C) == 0x3C && o[0] != this->remote_id_ && o[0] != 0xB5 &&
      o[0] != 0xB9) {
    ESP_LOGW(TAG, "Telecommande entendue avec l'identifiant 0x%02X (remote_id configure : 0x%02X)", o[0],
             this->remote_id_);
  }
  if (o[0] == this->remote_id_ && o.size() >= 5 && somme(o, 4) == o[4]) {
    this->nb_trames_rejetees_ -= o.size() >= 5;
    this->nb_trames_corrigees_ += corrigee;
    this->derniere_telecommande_ms_ = millis();
    this->etat_entendu_ = o[1];
    // Valeur brute du 2e octet (7 bits bas) : supposee temperature lue par la telecommande, non verifiee
    if (this->temperature_telecommande_ != nullptr) {
      float t = o[1] & 0x7F;
      if (!this->temperature_telecommande_->has_state() || this->temperature_telecommande_->state != t)
        this->temperature_telecommande_->publish_state(t);
    }
    return;
  }
  if (o[0] == 0xB5 && o.size() >= 32 && somme(o, 31) == o[31]) {
    this->nb_trames_rejetees_--;
    this->dernier_ecran_rejete_ms_ = ecran_rejete_avant;
    this->nb_trames_corrigees_ += corrigee;
    if (this->rssi_ != nullptr)
      this->rssi_->publish_state(rssi);
    this->derniere_reponse_ms_ = millis();
    this->dernier_b5_.assign(o.begin(), o.begin() + 32);
    this->traiter_ecran_(o);
    return;
  }
  if (o[0] == 0xB9 && o.size() >= 6 && somme(o, 5) == o[5]) {
    // Etat court = heure seule (la telecommande endormie le demande toutes les ~44 s)
    this->nb_trames_rejetees_--;
    this->nb_trames_corrigees_ += corrigee;
    this->derniere_reponse_ms_ = millis();
    // Le B9 porte les octets 1-4 de l'ecran (la ligne du bas) : heure sur l'ecran principal, valeur sur
    // STOP/START (B9 4F 46 46 20 = « OFF », 2026-11-24 18:20). Pendant une marche Energy Saving sur un ecran
    // de valeur, on l'applique au dernier ecran complet pour lire la nouvelle valeur.
    if (this->etape_ == Etape::ENS && this->dernier_ecran_.ens_ecran > 0 && this->dernier_b5_.size() == 32) {
      std::vector<uint8_t> e = this->dernier_b5_;
      std::copy(o.begin() + 1, o.begin() + 5, e.begin() + 1);
      e[31] = somme(e, 31);
      ESP_LOGD(TAG, "Energy Saving : ligne du bas par trame courte");
      this->dernier_b5_ = e;
      this->traiter_ecran_(e);
      return;
    }
    if (this->heure_ != nullptr && est_chiffre(o[1]) && est_chiffre(o[2]) && est_chiffre(o[3]) && est_chiffre(o[4])) {
      char h[12];
      snprintf(h, sizeof(h), "%d%d:%d%d", o[1] & 0x7F, o[2] & 0x7F, o[3] & 0x7F, o[4] & 0x7F);
      if (this->heure_->state != h)
        this->heure_->publish_state(h);
    }
  }
}

void PiazzettaComponent::traiter_ecran_(const std::vector<uint8_t> &o) {
  Ecran e;
  for (int i = 10; i < 19; i++)
    e.ligne1 += car_lcd(o[i]);
  for (int i = 19; i < 28; i++)
    e.ligne2 += car_lcd(o[i]);
  for (int i = 1; i < 10; i++)
    e.bas += car_lcd(o[i]);

  // Ecran de reglage : « REGLER » (francais) ou « SET » (anglais, « SET POWER », « SET VENT-1 »)
  e.menu = o[1] == 0x20 && (e.ligne1.find("REGLER") != std::string::npos || e.ligne1.find("SET") != std::string::npos);
  if (e.menu) {
    e.menu_marque = o[4];
    e.menu_valeur = o[5] & 0x7F;
    // SET ROOM TEMPERAT. : « 70F » en o[5..7] (B5 20 20 20 20 07 00 46 ..., lu le 2026-11-23)
    const uint8_t u = o[7] & 0x7F;
    if (e.ligne1.find("ROOM") != std::string::npos && est_chiffre(o[5]) && est_chiffre(o[6]) && (u == 'F' || u == 'C'))
      e.consigne = (o[5] & 0x7F) * 10 + (o[6] & 0x7F);
  } else {
    // L'ecran principal porte l'unite (F ou C) apres la temperature (o[5..9]). MEMORY COUNTERS (TOTAL HOURS
    // 22018, NUMBER OF STARTS 01033) a aussi des chiffres en o[1..5] mais pas d'unite : sans ce test, il
    // etait pris pour l'heure 22:01 et 8 F (2026-11-21 12:57), et polluait Phase/Etat/heure.
    bool unite = false;
    for (int i = 5; i < 10; i++)
      if ((o[i] & 0x7F) == 'F' || (o[i] & 0x7F) == 'C')
        unite = true;
    if (unite && est_chiffre(o[1]) && est_chiffre(o[2]) && est_chiffre(o[3]) && est_chiffre(o[4])) {
      e.heure_h = (o[1] & 0x7F) * 10 + (o[2] & 0x7F);
      e.heure_m = (o[3] & 0x7F) * 10 + (o[4] & 0x7F);
    }
    int t = 0, n = 0;
    for (int i = 5; i < 10; i++)
      if (est_chiffre(o[i]) && o[i] != 0x20) {
        t = t * 10 + (o[i] & 0x7F);
        n++;
      }
    if (n > 0 && e.heure_h >= 0)
      e.temperature = t;
    // « NIVEAU / P<n> » : le chiffre qui suit P est la puissance
    for (int i = 19; i < 27; i++)
      if (o[i] == 'P' && (o[i + 1] & 0x7F) >= 1 && (o[i + 1] & 0x7F) <= 5 &&
          (e.ligne1.find("NIVEAU") != std::string::npos || e.ligne1.find("LEVEL") != std::string::npos))
        e.puissance = o[i + 1] & 0x7F;
  }
  // ENERGY SAVING : ecran « STOP » ou « START », valeur en o[1..3] : « OFF », « 4F » (20 04 46), « [2F » (5B 02 46 ;
  // 5B = signe moins). Lu le 2026-11-21 et le 2026-11-23.
  e.bas_vide = true;
  for (int i = 1; i < 10; i++)
    if (o[i] != 0x20)
      e.bas_vide = false;
  {
    const std::string l1 = rogner(e.ligne1), l2 = rogner(e.ligne2);
    if ((l1 == "STOP" || l1 == "START") && l2.empty()) {
      const uint8_t a = o[1] & 0x7F, b = o[2] & 0x7F, c = o[3] & 0x7F;
      e.ens_ecran = l1 == "STOP" ? 1 : 2;
      if (a == 'O' && b == 'F' && c == 'F')
        e.ens_valeur = 0;
      else if ((c == 'F' || c == 'C') && b >= 1 && b <= 9)
        e.ens_valeur = b;
    }
  }
  // Icones : bargraphe de puissance (bits bas de o[29]) et ventilateur (o[30])
  if (e.puissance == 0) {
    int barres = __builtin_popcount(o[29] & 0x0F);
    if (barres >= 1 && barres <= 4)
      e.puissance = barres;
  }
  switch (o[30]) {
    case 0x08: e.ventilateur = 1; break;
    case 0x18: e.ventilateur = 2; break;
    case 0x39: e.ventilateur = 3; break;
    case 0x7B: e.ventilateur = 4; break;
    default: break;
  }
  if (e.menu && e.menu_marque == MARQUE_PUISSANCE)
    e.puissance = e.menu_valeur;
  if (e.menu && e.menu_marque == MARQUE_VENTILATEUR)
    e.ventilateur = e.menu_valeur;

  // Publication. Les lignes d'ecran suivent tout (menus compris) pour la telecommande deportee ;
  // « Phase » et « Etat » ne suivent que l'ecran principal, pour un historique lisible.
  auto publier = [](text_sensor::TextSensor *t, const std::string &v) {
    if (t != nullptr && t->state != v)
      t->publish_state(v);
  };
  publier(this->ligne1_, rogner(e.ligne1));
  publier(this->ligne2_, rogner(e.ligne2));
  std::string bas = rogner(e.bas);
  if (!e.menu && e.heure_h >= 0) {
    char b[32];
    if (e.temperature > -1000)
      snprintf(b, sizeof(b), "%02d:%02d   %d°", e.heure_h, e.heure_m, e.temperature);
    else
      snprintf(b, sizeof(b), "%02d:%02d", e.heure_h, e.heure_m);
    bas = b;
  }
  publier(this->bas_, bas);
  std::string phase = rogner(e.ligne1 + " " + e.ligne2);
  // Ecran principal = pas un menu ET une heure valide en bas a gauche. Les listes de menus, la liste des
  // parametres, KEY ACCESS et STOVE STATUS n'ont pas d'heure : ils ne polluent plus Phase/Etat (2026-11-20).
  const bool principal = !e.menu && e.heure_h >= 0;
  if (principal) {
    publier(this->phase_, phase);
    publier(this->etat_, etat_de(phase));
  }
  // STOVE STATUS (« RPM  AUG ») : ligne 2 = tours/min extracteur (o[19..22]) + vis en s (o[23..27], point
  // decimal = bit 7) ; bas = temperature des fumees (o[1..3]) puis temperature ambiante. Lu le 2026-11-20.
  if (e.ligne1.find("RPM") != std::string::npos && e.ligne1.find("AUG") != std::string::npos) {
    auto nombre = [&](int de, int a, float &v) {
      float val = 0, div = 1;
      bool vu = false, decimales = false;
      for (int i = de; i < a; i++) {
        uint8_t c = o[i] & 0x7F;
        if (c == 0x20 && !vu)
          continue;
        if (c > 9)
          break;
        val = val * 10 + c;
        if (decimales)
          div *= 10;
        vu = true;
        if (o[i] & 0x80)
          decimales = true;
      }
      v = val / div;
      return vu;
    };
    float v;
    if (this->rpm_fumees_ != nullptr && nombre(19, 23, v))
      this->rpm_fumees_->publish_state(v);
    if (this->vis_ != nullptr && nombre(23, 28, v))
      this->vis_->publish_state(v);
    if (this->fumees_ != nullptr && nombre(1, 5, v))
      this->fumees_->publish_state(v);
  }
  if (this->temperature_ != nullptr && e.temperature > -1000) {
    // 2026-11-20 : ~15 min apres un arret a la telecommande (07:56, CLEANING GRATE) le poele s'est mis a envoyer
    // 32 F (0 C) et l'a garde a l'arret puis pendant START PHASE I, piece a 62-66 F. 32 F / 0 C = pas de mesure
    // (NAN), quel que soit l'etat : une piece chauffee n'est jamais a 0 C.
    bool fahrenheit = false;
    for (int i = 5; i < 10; i++)
      if ((o[i] & 0x7F) == 'F')
        fahrenheit = true;
    const bool factice = e.temperature == (fahrenheit ? 32 : 0);
    if (factice) {
      if (!this->temperature_->has_state() || !std::isnan(this->temperature_->state))
        this->temperature_->publish_state(NAN);
    } else if (!this->temperature_->has_state() || this->temperature_->state != e.temperature) {
      this->temperature_->publish_state(e.temperature);
    }
  }
  if (this->heure_ != nullptr && e.heure_h >= 0) {
    char h[12];
    snprintf(h, sizeof(h), "%02d:%02d", e.heure_h, e.heure_m);
    if (this->heure_->state != h)
      this->heure_->publish_state(h);
  }
  if (this->puissance_ != nullptr && e.puissance > 0 && (!this->puissance_->has_state() || this->puissance_->state != e.puissance))
    this->puissance_->publish_state(e.puissance);
  if (this->ventilateur_ != nullptr && e.ventilateur > 0 &&
      (!this->ventilateur_->has_state() || this->ventilateur_->state != e.ventilateur))
    this->ventilateur_->publish_state(e.ventilateur);
  // Valeurs reglees, vues seulement sur les ecrans SET : c'est ce que HA compare a ses cibles
  if (e.consigne > 0) {
    this->derniere_consigne_ = e.consigne;
    if (this->consigne_ != nullptr && (!this->consigne_->has_state() || this->consigne_->state != e.consigne))
      this->consigne_->publish_state(e.consigne);
  }
  if (e.menu && e.menu_marque == MARQUE_PUISSANCE && e.menu_valeur >= 1 && e.menu_valeur <= 5 &&
      this->puissance_reglee_ != nullptr &&
      (!this->puissance_reglee_->has_state() || this->puissance_reglee_->state != e.menu_valeur))
    this->puissance_reglee_->publish_state(e.menu_valeur);
  if (e.menu && e.menu_marque == MARQUE_VENTILATEUR && e.menu_valeur >= 1 && e.menu_valeur <= 5 &&
      this->ventilateur_regle_ != nullptr &&
      (!this->ventilateur_regle_->has_state() || this->ventilateur_regle_->state != e.menu_valeur))
    this->ventilateur_regle_->publish_state(e.menu_valeur);

  if (e.ens_ecran > 0 && e.ens_valeur >= 0 && e.ens_valeur <= 6 && e.ens_valeur % 2 == 0) {
    PiazzettaSelect *sel = e.ens_ecran == 1 ? this->ens_stop_ : this->ens_start_;
    (e.ens_ecran == 1 ? this->ens_stop_vu_ : this->ens_start_vu_) = e.ens_valeur;
    if (sel != nullptr)
      sel->publish_state((size_t) (e.ens_valeur / 2));
  }

  this->dernier_ecran_ = e;
  this->ecran_recu_ = true;
  if (principal) {
    this->ecran_principal_recu_ = true;
    this->dernier_ecran_principal_ms_ = millis();
    const std::string etat = etat_de(phase);
    // Confirmation d'une bascule : un Etat connu, different de celui d'avant l'appui
    if (this->bascule_en_attente_ && etat != ETAT_AUTRE && etat != this->etat_avant_bascule_) {
      this->bascule_en_attente_ = false;
      ESP_LOGI(TAG, "Bascule confirmee : %s -> %s", this->etat_avant_bascule_.c_str(), etat.c_str());
      this->publier_bascule_();
    }
    this->dernier_etat_ = etat;
    const bool stable = etat == ETAT_MARCHE || etat == ETAT_ARRET;
    this->prochaine_synchro_ms_ =
        millis() + (this->bascule_en_attente_ ? RESYNCHRO_BASCULE_MS : stable ? RESYNCHRO_STABLE_MS : RESYNCHRO_TRANSITION_MS);
  }
  ESP_LOGI(TAG, "Ecran : %s", phase.c_str());

  // Suite de la tache de reglage en cours
  if (this->etape_ == Etape::ENS) {
    this->avancer_ens_(e, principal, true);
    return;
  }
  if (this->etape_ != Etape::ATTENTE_MENU && this->etape_ != Etape::ATTENTE_VALEUR)
    return;
  int valeur = this->valeur_reglage_(e);
  if (valeur > 0) {
    // Notre ecran SET est ouvert
    const int sens = valeur < this->cible_ ? 1 : -1;
    if (valeur == this->cible_) {
      this->terminer_tache_("cible atteinte");
    } else if (this->sens_ != 0 && sens != this->sens_) {
      // Consigne : pas de 1 C, donc 67 F n'existe pas ; on s'arrete au passage au lieu d'osciller
      this->terminer_tache_("cible inatteignable, arret au plus proche");
    } else if (this->pas_ >= PAS_MAX) {
      this->terminer_tache_("trop de pas, abandon");
    } else {
      this->pas_++;
      this->sens_ = sens;
      this->planifier_({this->remote_id_, this->octet_etat_(ETAT_MENU), 0x00, this->touche_(sens > 0 ? TOUCHE_PLUS : TOUCHE_MOINS)});
      this->etape_ = Etape::ATTENTE_VALEUR;
      this->echeance_ms_ = this->emission_a_ms_ + DELAI_REPONSE_MS;
    }
    return;
  }
  // Ecran principal (ou autre menu) : on (r)ouvre notre menu. Plus de raccourci « deja a la cible » sur
  // l'ecran principal : en marche, la puissance affichee est le niveau module (P1 avec P4 regle, 2026-11-22),
  // seul l'ecran SET montre la valeur reglee. La touche de menu bascule, d'ou les essais.
  if (++this->essais_ > ESSAIS_MAX) {
    this->terminer_tache_("menu impossible a ouvrir, abandon");
    return;
  }
  this->planifier_({this->remote_id_, this->octet_etat_(ETAT_REPOS), 0x00, this->touche_(this->touche_menu_())});
  this->etape_ = Etape::ATTENTE_MENU;
  this->echeance_ms_ = this->emission_a_ms_ + DELAI_REPONSE_MS;
}

uint8_t PiazzettaComponent::touche_menu_() const {
  switch (this->reglage_) {
    case Reglage::PUISSANCE: return TOUCHE_PUISSANCE;
    case Reglage::CONSIGNE: return TOUCHE_CONSIGNE;
    default: return TOUCHE_VENTILATEUR;
  }
}

// Valeur du reglage en cours si l'ecran est son ecran SET, sinon 0
int PiazzettaComponent::valeur_reglage_(const Ecran &e) const {
  switch (this->reglage_) {
    case Reglage::CONSIGNE: return e.consigne;
    case Reglage::PUISSANCE: return e.menu && e.menu_marque == MARQUE_PUISSANCE ? e.menu_valeur : 0;
    default: return e.menu && e.menu_marque == MARQUE_VENTILATEUR ? e.menu_valeur : 0;
  }
}

// ---------------------------------------------------------------- Energy Saving

// Rang des valeurs dans l'ordre de la touche + : STOP OFF, 2, 4, 6 (vu OFF > 2F > 4F) ;
// START -2, -4, -6, OFF puis -2 (vu -6F > OFF > -2F ; le reste du cycle est suppose). Le sens choisi
// evite de passer par START OFF (plus de rallumage) quand c'est possible.
static int rang_ens(bool stop, int v) { return stop ? v / 2 : (v == 0 ? 3 : v / 2 - 1); }

void PiazzettaSelect::control(size_t index) {
  if (this->parent_ == nullptr)
    return;
  const int v = (int) index * 2;
  this->parent_->demander_ens(this->genre_ == 0 ? v : -1, this->genre_ == 1 ? v : -1);
}

void PiazzettaComponent::demander_ens(int stop, int start) {
  if (this->etape_ != Etape::REPOS) {
    ESP_LOGW(TAG, "Energy Saving ignore : une commande est deja en cours");
    return;
  }
  if (start == 0) {
    ESP_LOGW(TAG, "Energy Saving : START OFF refuse (le poele ne se rallumerait plus)");
    if (this->ens_start_ != nullptr && this->ens_start_vu_ >= 0)
      this->ens_start_->publish_state((size_t) (this->ens_start_vu_ / 2));
    return;
  }
  if (this->dernier_etat_ == ETAT_ALLUMAGE || this->dernier_etat_ == ETAT_NETTOYAGE) {
    ESP_LOGW(TAG, "Energy Saving refuse : poele en %s", this->dernier_etat_.c_str());
    return;
  }
  if (this->dernier_ecran_rejete_ms_ != 0 && millis() - this->dernier_ecran_rejete_ms_ < CALME_ENS_MS) {
    ESP_LOGW(TAG, "Energy Saving refuse : ecran brouille il y a %u s (attendre %u s sans trame rejetee)",
             (unsigned) ((millis() - this->dernier_ecran_rejete_ms_) / 1000), (unsigned) (CALME_ENS_MS / 1000));
    return;
  }
  const Ecran &d = this->dernier_ecran_;
  if (d.menu || d.heure_h < 0 || millis() - this->dernier_ecran_principal_ms_ > 90000) {
    ESP_LOGW(TAG, "Energy Saving refuse : ecran principal pas a jour (interroger le poele d'abord)");
    return;
  }
  ESP_LOGI(TAG, "Energy Saving -> STOP %d, START %d (-1 = inchange)", stop, start);
  this->cible_ens_stop_ = stop;
  this->cible_ens_start_ = start;
  this->etape_ = Etape::ENS;
  this->ens_etape_ = 0;
  this->ens_appuis_ = 0;
  this->ens_attente_ = false;
  this->pas_ = 0;
  this->essais_ = 0;
  this->avancer_ens_(d, true, false);
}

void PiazzettaComponent::touche_ens_(uint8_t base) {
  this->ens_trame_ = {this->remote_id_, this->octet_etat_(ETAT_MENU), 0x80, this->touche_(base)};
  this->planifier_(this->ens_trame_);
  this->ens_attente_ = true;
  this->echeance_ms_ = this->emission_a_ms_ + DELAI_REPONSE_MS;
}

void PiazzettaComponent::avancer_ens_(const Ecran &e, bool principal, bool reponse) {
  if (reponse) {
    // Ecran non sollicite (repetition, vraie telecommande) : on attend la reponse a notre touche
    if (!this->ens_attente_)
      return;
    this->ens_attente_ = false;
    this->essais_ = 0;
  }
  auto abandon = [this](const char *bilan) {
    this->terminer_tache_(bilan);
    // Un cran en arriere sans valider ; le menu se referme seul ensuite
    this->planifier_({this->remote_id_, this->octet_etat_(ETAT_MENU), 0x80, this->touche_(PORTE_ESC)});
  };
  const std::string l1 = rogner(e.ligne1), l2 = rogner(e.ligne2);
  if (principal) {
    if (this->ens_etape_ == 2) {
      this->terminer_tache_("Energy Saving reglee");
    } else if (this->ens_etape_ == 1 || this->ens_appuis_ > 0) {
      this->terminer_tache_("Energy Saving : menu referme avant la fin, abandon");
    } else {
      this->touche_ens_(PORTE_MENU);
    }
    return;
  }
  if (e.ens_ecran > 0) {
    this->ens_etape_ = std::max(this->ens_etape_, 1);
    const bool stop = e.ens_ecran == 1;
    const int cible = stop ? this->cible_ens_stop_ : this->cible_ens_start_;
    const int v = e.ens_valeur;
    if (v < 0) {
      abandon("Energy Saving : valeur illisible, abandon");
      return;
    }
    if (cible < 0 || v == cible) {
      if (!stop)
        this->ens_etape_ = 2;  // SET sur START valide et revient a l'ecran principal
      this->touche_ens_(PORTE_SET);
      return;
    }
    if (++this->pas_ > PAS_MAX) {
      abandon("Energy Saving : trop de pas, abandon");
      return;
    }
    this->touche_ens_(rang_ens(stop, cible) > rang_ens(stop, v) ? PORTE_PLUS : PORTE_MOINS);
    return;
  }
  if (e.bas_vide && !l1.empty()) {
    if (this->ens_etape_ >= 1) {
      abandon("Energy Saving : retour inattendu a la liste, abandon");
      return;
    }
    if (l1 == "ENERGY" && l2 == "SAVING") {
      this->touche_ens_(PORTE_SET);
      return;
    }
    if (++this->ens_appuis_ > ENS_APPUIS_LISTE_MAX) {
      abandon("Energy Saving : ENERGY SAVING introuvable, abandon");
      return;
    }
    this->touche_ens_(PORTE_PLUS);
    return;
  }
  ESP_LOGW(TAG, "Energy Saving : ecran inattendu %s / %s", l1.c_str(), l2.c_str());
  abandon("Energy Saving : ecran inattendu, abandon");
}

// ---------------------------------------------------------------- Taches

void PiazzettaComponent::demander_reglage(Reglage reglage, int cible) {
  const char *nom = reglage == Reglage::PUISSANCE ? "puissance" : reglage == Reglage::CONSIGNE ? "consigne" : "ventilateur";
  if (this->etape_ != Etape::REPOS) {
    ESP_LOGW(TAG, "Reglage %s -> %d ignore : une commande est deja en cours", nom, cible);
    return;
  }
  ESP_LOGI(TAG, "Reglage %s -> %d", nom, cible);
  this->reglage_ = reglage;
  this->cible_ = cible;
  this->essais_ = 0;
  this->pas_ = 0;
  this->sens_ = 0;
  // Une touche provoque toujours un ecran complet, contrairement a une interrogation (heure seule
  // si rien n'a change) : on commence donc par la touche de menu, l'ecran recu guide la suite.
  this->planifier_({this->remote_id_, this->octet_etat_(ETAT_REPOS), 0x00, this->touche_(this->touche_menu_())});
  this->etape_ = Etape::ATTENTE_MENU;
  this->echeance_ms_ = this->emission_a_ms_ + DELAI_REPONSE_MS;
}

// Touches envoyees depuis HA (action press_keys). Bornes : jamais la famille marche/arret
// (touche & 0x1F == 0x01 : 01 21 41 61 81 A1 ...), une commande a la fois, 1 a 20 touches.
void PiazzettaComponent::appuyer_touches(const std::vector<int32_t> &touches, int ecart_ms, bool porte) {
  this->porte_seq_ = porte;
  if (this->ecoute_seule_) {
    ESP_LOGW(TAG, "Touches refusees : emission coupee (interrupteur Transmit)");
    return;
  }
  if (this->etape_ != Etape::REPOS || this->trame_en_attente_) {
    ESP_LOGW(TAG, "Touches refusees : une commande est deja en cours");
    return;
  }
  if (touches.empty() || touches.size() > 20) {
    ESP_LOGW(TAG, "Touches refusees : %u touches (1 a 20)", (unsigned) touches.size());
    return;
  }
  for (int32_t t : touches) {
    if (t < 0 || t > 0xFF) {
      ESP_LOGW(TAG, "Touches refusees : valeur %d hors 0..255", (int) t);
      return;
    }
    if ((t & 0x1F) == 0x01) {
      ESP_LOGW(TAG, "Touches refusees : 0x%02X est une touche marche/arret (bouton On-off seulement)", (unsigned) t);
      return;
    }
  }
  this->touches_seq_.clear();
  for (int32_t t : touches)
    this->touches_seq_.push_back((uint8_t) t);
  this->ecart_seq_ms_ = (uint32_t) std::max(150, std::min(ecart_ms, 3000));
  this->pas_ = 0;
  this->etape_ = Etape::SEQUENCE;
  this->echeance_ms_ = millis();
  ESP_LOGI(TAG, "Touches depuis HA : %s (ecart %u ms)", hexa(this->touches_seq_).c_str(), (unsigned) this->ecart_seq_ms_);
}

// Sequence de l'appui long « On-off » depuis HA, ex. « 21* 21* 41 61 61* » (* = bit 7 de l'octet d'etat).
// Seules les touches de la famille marche/arret sont acceptees. Texte invalide : sequence precedente conservee.
bool PiazzettaComponent::regler_sequence_maintien(const std::string &texte) {
  std::vector<uint8_t> touches, etats;
  size_t i = 0;
  while (i < texte.size()) {
    while (i < texte.size() && (texte[i] == ' ' || texte[i] == ','))
      i++;
    if (i >= texte.size())
      break;
    size_t j = i;
    while (j < texte.size() && texte[j] != ' ' && texte[j] != ',')
      j++;
    std::string jeton = texte.substr(i, j - i);
    i = j;
    bool bit7 = !jeton.empty() && jeton.back() == '*';
    if (bit7)
      jeton.pop_back();
    char *fin = nullptr;
    long v = strtol(jeton.c_str(), &fin, 16);
    if (jeton.empty() || fin == nullptr || *fin != '\0' || v < 0 || v > 0xFF || (v & 0x1F) != 0x01) {
      ESP_LOGW(TAG, "Sequence On-off refusee : jeton « %s » (attendu 21, 41*, 61 ...)", jeton.c_str());
      return false;
    }
    touches.push_back((uint8_t) v);
    etats.push_back(bit7 ? 0xBE : 0x3E);
  }
  if (touches.empty() || touches.size() > 30) {
    ESP_LOGW(TAG, "Sequence On-off refusee : %u touches (1 a 30)", (unsigned) touches.size());
    return false;
  }
  this->touches_maintien_ = touches;
  this->etats_maintien_ = etats;
  ESP_LOGI(TAG, "Sequence On-off : %s (%u trames)", texte.c_str(), (unsigned) touches.size());
  return true;
}

// Emission differee : au plus tot delai_touche_ms_ apres la fin de la derniere trame recue
// (le poele ignorait les touches envoyees 9 a 13 ms apres son ecran, 2026-11-19).
void PiazzettaComponent::planifier_(std::vector<uint8_t> octets) {
  uint32_t maintenant = millis();
  uint32_t au_plus_tot = this->derniere_reception_ms_ + this->delai_touche_ms_;
  this->emission_a_ms_ = (int32_t) (au_plus_tot - maintenant) > 0 ? au_plus_tot : maintenant;
  this->trame_planifiee_ = std::move(octets);
  this->trame_en_attente_ = true;
}

void PiazzettaComponent::demander_action(Action action) {
  if (this->etape_ != Etape::REPOS) {
    ESP_LOGW(TAG, "Action ignoree : une commande est deja en cours");
    return;
  }
  if (action == Action::INTERROGER) {
    this->emettre_octets_({this->remote_id_, this->octet_etat_(ETAT_REPOS), 0x00, this->touche_interrogation_});
    this->derniere_interrogation_ms_ = millis();
    return;
  }
  // Marche/arret = appui long sur ⏻ : touche 81, puis 61 repetee tant que le bouton est « maintenu ».
  // Valide : extinction et allumage declenches par l'ESP sur le poele de reference.
  const uint32_t ecart = millis() - this->dernier_marche_arret_ms_;
  if (this->marche_arret_envoye_ && ecart < ANTI_REBOND_MARCHE_ARRET_MS) {
    ESP_LOGW(TAG, "Marche/arret ignore : appui precedent il y a %u s (anti-rebond %u s)", (unsigned) (ecart / 1000),
             (unsigned) (ANTI_REBOND_MARCHE_ARRET_MS / 1000));
    return;
  }
  if (this->bascule_en_attente_ && ecart < VERROU_BASCULE_MAX_MS) {
    ESP_LOGW(TAG, "Marche/arret ignore : la bascule d'il y a %u s n'est pas encore confirmee par l'ecran",
             (unsigned) (ecart / 1000));
    return;
  }
  this->marche_arret_envoye_ = true;
  this->dernier_marche_arret_ms_ = millis();
  this->bascule_en_attente_ = true;
  this->etat_avant_bascule_ = this->dernier_etat_;
  this->publier_bascule_();
  ESP_LOGW(TAG, "Marche/arret : appui long simule");
  this->emettre_octets_({this->remote_id_, this->octet_etat_(0x3E), 0x00, TOUCHE_MARCHE});
  this->pas_ = 0;
  this->etape_ = Etape::APPUI_LONG;
  this->echeance_ms_ = millis() + this->periode_maintien_ms_;
}

void PiazzettaComponent::avancer_tache_() {
  if (this->etape_ == Etape::REPOS || (int32_t) (millis() - this->echeance_ms_) < 0)
    return;
  if (this->etape_ == Etape::SYNCHRO) {
    // Le menu ventilateur s'est referme : l'ecran principal a « change », il revient en entier
    this->etape_ = Etape::REPOS;
    this->derniere_interrogation_ms_ = millis();
    this->emettre_octets_({this->remote_id_, this->octet_etat_(ETAT_REPOS), 0x00, this->touche_interrogation_});
    return;
  }
  if (this->etape_ == Etape::APPUI_LONG) {
    // Sequence YAML (MULTICOMFORT V5 : 21 21 41 61 61 ...) ou, a defaut, touche fixe + ETATS_MAINTIEN (V3)
    const bool seq = !this->touches_maintien_.empty();
    const int nb = seq ? (int) this->touches_maintien_.size() : APPUIS_MAINTIEN;
    if (this->pas_ >= nb) {
      this->terminer_tache_("appui long termine");
      this->emettre_octets_({this->remote_id_, this->octet_etat_(ETAT_REPOS), 0x00, this->touche_interrogation_});
      this->prochaine_synchro_ms_ = millis() + RESYNCHRO_APRES_MARCHE_ARRET_MS;
      return;
    }
    const uint8_t touche = seq ? this->touches_maintien_[this->pas_] : this->touche_maintien_;
    const uint8_t etat = seq ? this->etats_maintien_[this->pas_] : ETATS_MAINTIEN[this->pas_];
    this->emettre_octets_({this->remote_id_, this->octet_etat_(etat), 0x00, touche});
    this->pas_++;
    this->echeance_ms_ = millis() + this->periode_maintien_ms_;
    return;
  }
  if (this->etape_ == Etape::SEQUENCE) {
    if (this->pas_ >= (int) this->touches_seq_.size()) {
      this->terminer_tache_("touches HA envoyees");
      return;
    }
    const uint8_t touche = this->touches_seq_[this->pas_++];
    // 3e octet 0x80 = trappe de la telecommande ouverte (MULTICOMFORT V5) : les memes codes deviennent
    // MENU 84, SET 98, ESC 90, OPT 8A (capture du 2026-10-31)
    // Trappe ouverte : bit 7 de l'octet d'etat toujours a 1, comme la vraie telecommande (BF ... 80 xx, 2026-10-31)
    this->planifier_({this->remote_id_,
                      this->octet_etat_(this->porte_seq_ || this->dernier_ecran_.menu ? ETAT_MENU : ETAT_REPOS),
                      (uint8_t) (this->porte_seq_ ? 0x80 : 0x00), touche});
    this->echeance_ms_ = this->emission_a_ms_ + this->ecart_seq_ms_;
    return;
  }
  if (this->etape_ == Etape::ENS) {
    // Pas d'ecran : touche perdue, ou reponse perdue. On renvoie la MEME trame (meme compteur) : si la touche
    // etait perdue elle agit maintenant ; si seule la reponse l'etait, le poele ignore le doublon et renvoie
    // l'ecran courant. Jamais deux SET pour un : le 2026-11-23 03:23, redecider depuis l'ecran precedent avait
    // valide STOP a l'aveugle (sans gravite, c'etait deja la cible).
    // Sauf apres + ou - sur STOP/START : le doublon n'est PAS toujours ignore (2026-11-24 18:20, quatre renvois
    // de + ont fait 6F > OFF > 2F). On interroge sans touche : le poele renvoie la ligne du bas (B9), et la
    // marche repart de la valeur reellement affichee.
    if (++this->essais_ > ESSAIS_MAX) {
      this->terminer_tache_("Energy Saving : pas de reponse du poele, abandon");
      return;
    }
    const uint8_t k = this->ens_trame_.size() >= 4 ? (uint8_t) (this->ens_trame_[3] & 0x9F) : 0;
    if (this->dernier_ecran_.ens_ecran > 0 && (k == PORTE_PLUS || k == PORTE_MOINS)) {
      ESP_LOGD(TAG, "Energy Saving : pas d'ecran apres +/-, interrogation sans touche (essai %d)", this->essais_);
      this->planifier_({this->remote_id_, this->octet_etat_(ETAT_MENU), 0x80, this->touche_interrogation_});
      this->ens_attente_ = true;
      this->echeance_ms_ = this->emission_a_ms_ + DELAI_REPONSE_MS;
      return;
    }
    ESP_LOGD(TAG, "Energy Saving : pas d'ecran, meme trame renvoyee (essai %d)", this->essais_);
    this->planifier_(this->ens_trame_);
    this->ens_attente_ = true;
    this->echeance_ms_ = this->emission_a_ms_ + DELAI_REPONSE_MS;
    return;
  }
  // Pas de reponse : on renvoie la touche de menu ; l'ecran complet qu'elle provoque relancera la tache
  if (++this->essais_ > ESSAIS_MAX) {
    this->terminer_tache_("pas de reponse du poele, abandon");
    return;
  }
  ESP_LOGD(TAG, "Pas de reponse, nouvel appui sur la touche de menu (essai %d)", this->essais_);
  this->planifier_({this->remote_id_, this->octet_etat_(ETAT_REPOS), 0x00, this->touche_(this->touche_menu_())});
  this->etape_ = Etape::ATTENTE_MENU;
  this->echeance_ms_ = this->emission_a_ms_ + DELAI_REPONSE_MS;
}

void PiazzettaComponent::terminer_tache_(const char *bilan) {
  ESP_LOGI(TAG, "Fin de commande : %s", bilan);
  this->etape_ = Etape::REPOS;
  this->derniere_interrogation_ms_ = millis();
  // Remet l'affichage HA sur la valeur reellement connue (utile en cas d'echec)
  if (this->puissance_ != nullptr && this->dernier_ecran_.puissance > 0)
    this->puissance_->publish_state(this->dernier_ecran_.puissance);
  if (this->ventilateur_ != nullptr && this->dernier_ecran_.ventilateur > 0)
    this->ventilateur_->publish_state(this->dernier_ecran_.ventilateur);
  if (this->consigne_ != nullptr && this->derniere_consigne_ > 0)
    this->consigne_->publish_state(this->derniere_consigne_);
}

// ---------------------------------------------------------------- Cycle de vie

void PiazzettaComponent::setup() {
  // Etat initial des interrupteurs avant toute emission possible (par defaut ALWAYS_OFF : ecoute seule au demarrage)
  if (this->emission_ != nullptr) {
    auto init = this->emission_->get_initial_state_with_restore_mode();
    this->regler_option(Option::EMISSION, init.has_value() ? *init : !this->ecoute_seule_);
  }
  if (this->synchro_auto_sw_ != nullptr) {
    auto init = this->synchro_auto_sw_->get_initial_state_with_restore_mode();
    this->regler_option(Option::SYNCHRO_AUTO, init.has_value() ? *init : this->synchro_auto_);
  }
  s_pin = this->dio2_;
  s_spi = new SPIClass(VSPI);
  s_spi->begin(this->sck_, this->miso_, this->mosi_, this->cs_);
  s_module = new Module(this->cs_, this->dio0_, this->rst_, this->dio1_, *s_spi);
  s_radio = new SX1278(s_module);
  this->durees_.reserve(MAX_DUREES);
  this->configurer_radio_();
  this->ecouter_();
  this->boucle_rapide_.start();
  // la boucle doit tourner souvent pour suivre le RSSI des trames
  this->demarrage_ms_ = millis();
  this->prochaine_synchro_ms_ = millis() + DELAI_AVANT_SYNCHRO_MS;
  this->publier_bascule_();
  // Premiere interrogation peu apres le demarrage, pour se resynchroniser
  this->derniere_interrogation_ms_ = millis() - this->intervalle_ms_ + 5000;
}

void PiazzettaComponent::loop() {
  this->traiter_fronts_();

  uint32_t t = maintenant_us();
  if (this->en_trame_) {
    // RSSI lu seulement pendant une impulsion : une lecture SPI dans un silence fait apparaitre une
    // impulsion parasite sur DIO2 (presque tous les parasites observes tombaient a la cadence de ces lectures)
    if (this->dernier_niveau_ == 1 && t - this->dernier_rssi_us_ > 1000) {
      float r = s_radio->getRSSI(false, true);
      if (r > this->rssi_max_)
        this->rssi_max_ = r;
      if (r >= this->seuil_rssi_)
        this->dernier_fort_ = t;
      this->dernier_rssi_us_ = t;
    }
    // Fin de trame : silence sur la broche, ou plus de signal fort (le bruit continue de basculer)
    if (this->dernier_niveau_ == 0 && t - this->dernier_front_ > FIN_TRAME_US)
      this->finir_trame_();
    else if (t - this->dernier_fort_ > FIN_TRAME_US)
      this->finir_trame_();
    else if (t - this->debut_trame_ > TRAME_MAX_US)
      this->finir_trame_();
    return;  // rien d'autre pendant une reception
  }

  // Trame planifiee (delai apres reception) : emise des que son heure est venue, rien d'autre avant
  if (this->trame_en_attente_) {
    if ((int32_t) (millis() - this->emission_a_ms_) < 0)
      return;
    this->trame_en_attente_ = false;
    this->emettre_octets_(this->trame_planifiee_);
    return;
  }

  this->avancer_tache_();

  uint32_t ms = millis();
  // Synchronisation si l'ecran principal n'a pas ete revu a temps (au demarrage, ou trame manquee) :
  // chaque ecran principal recu repousse l'echeance
  if (this->synchro_auto_ && this->etape_ == Etape::REPOS && (int32_t) (ms - this->prochaine_synchro_ms_) >= 0 &&
      ms - this->derniere_telecommande_ms_ > SILENCE_TELECOMMANDE_MS) {
    ESP_LOGI(TAG, "Synchronisation : ouverture du menu ventilateur pour obtenir l'ecran complet");
    this->emettre_octets_({this->remote_id_, this->octet_etat_(ETAT_REPOS), 0x00, TOUCHE_VENTILATEUR});
    this->etape_ = Etape::SYNCHRO;
    this->echeance_ms_ = ms + DUREE_SYNCHRO_MS;
    this->prochaine_synchro_ms_ = ms + RESYNCHRO_TRANSITION_MS;  // nouvel essai si l'ecran ne revient pas
    return;
  }
  // Interrogation periodique, sauf si la vraie telecommande parle (ses reponses suffisent)
  if (this->etape_ == Etape::REPOS && ms - this->derniere_interrogation_ms_ > this->intervalle_ms_ &&
      ms - this->derniere_telecommande_ms_ > SILENCE_TELECOMMANDE_MS) {
    this->derniere_interrogation_ms_ = ms;
    this->emettre_octets_({this->remote_id_, this->octet_etat_(ETAT_REPOS), 0x00, this->touche_interrogation_});
  }
  if (this->joignable_ != nullptr && ms - this->dernier_joignable_ms_ > 5000) {
    this->dernier_joignable_ms_ = ms;
    bool ok = this->derniere_reponse_ms_ != 0 && ms - this->derniere_reponse_ms_ < DELAI_JOIGNABLE_MS;
    if (!this->joignable_->has_state() || this->joignable_->state != ok)
      this->joignable_->publish_state(ok);
  }
  // Verrou de bascule expire sans confirmation : la commande est probablement restee sans effet
  if (this->bascule_en_attente_ && ms - this->dernier_marche_arret_ms_ >= VERROU_BASCULE_MAX_MS) {
    this->bascule_en_attente_ = false;
    ESP_LOGW(TAG, "Bascule jamais confirmee par l'ecran en %u min : verrou leve", (unsigned) (VERROU_BASCULE_MAX_MS / 60000));
    this->publier_bascule_();
  }
  if (ms - this->dernier_diagnostic_ms_ >= PERIODE_DIAGNOSTIC_MS) {
    this->dernier_diagnostic_ms_ = ms;
    // Age de l'ecran principal (depuis le demarrage s'il n'en est encore venu aucun)
    if (this->age_ecran_ != nullptr)
      this->age_ecran_->publish_state(
          (ms - (this->ecran_principal_recu_ ? this->dernier_ecran_principal_ms_ : this->demarrage_ms_)) / 1000);
    if (this->trames_rejetees_ != nullptr)
      this->trames_rejetees_->publish_state(this->nb_trames_rejetees_);
    if (this->trames_corrigees_ != nullptr)
      this->trames_corrigees_->publish_state(this->nb_trames_corrigees_);
  }
}

void PiazzettaComponent::publier_bascule_() {
  if (this->bascule_attente_ != nullptr)
    this->bascule_attente_->publish_state(this->bascule_en_attente_);
}

void PiazzettaComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Poele Piazzetta (radio SX1278)");
  ESP_LOGCONFIG(TAG, "  Broches : SCK %d, MISO %d, MOSI %d, CS %d, RST %d, DIO0 %d, DIO1 %d, DIO2 %d", this->sck_,
                this->miso_, this->mosi_, this->cs_, this->rst_, this->dio0_, this->dio1_, this->dio2_);
  ESP_LOGCONFIG(TAG, "  Identifiant telecommande : 0x%02X", this->remote_id_);
  ESP_LOGCONFIG(TAG, "  Touches : interrogation 0x%02X, maintien 0x%02X ; octet d'etat %s", this->touche_interrogation_,
                this->touche_maintien_, this->copier_etat_ ? "copie de la telecommande" : "fixe");
  ESP_LOGCONFIG(TAG, "  Emission : %s ; synchronisation automatique : %s",
                this->ecoute_seule_ ? "ECOUTE SEULE (aucune emission)" : "AUTORISEE",
                this->synchro_auto_ ? "active" : "inactive");
  ESP_LOGCONFIG(TAG, "  Frequence : %.3f MHz, emission %d dBm, seuil %d dBm, interrogation toutes les %u s",
                this->frequence_, this->puissance_emission_, this->seuil_rssi_, (unsigned) (this->intervalle_ms_ / 1000));
}

}  // namespace piazzetta
}  // namespace esphome
