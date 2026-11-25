#pragma once

// Poele Piazzetta pilote par radio 433 MHz, a la place de la telecommande F0203-1480V3.
// Le code radio reprend le banc de test (test-bench/firmware), valide sur le poele de reference.
// English: see docs/esphome.md and docs/protocol.md.

#include <algorithm>
#include <string>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/button/button.h"
#include "esphome/components/number/number.h"
#include "esphome/components/select/select.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/text_sensor/text_sensor.h"

namespace esphome {
namespace piazzetta {

// CONSIGNE = SET ROOM TEMPERAT. (touche 0x90 hors trappe), en degres affiches (pas de 1 C)
enum class Reglage : uint8_t { PUISSANCE, VENTILATEUR, CONSIGNE };
enum class Action : uint8_t { INTERROGER, MARCHE_ARRET };
// Interrupteurs HA : EMISSION = autoriser l'emission (inverse d'ecoute_seule),
// SYNCHRO_AUTO = appui automatique sur la touche ventilateur/menu pour obtenir l'ecran complet
enum class Option : uint8_t { EMISSION, SYNCHRO_AUTO };

class PiazzettaComponent;

class PiazzettaNumber : public number::Number {
 public:
  void set_parent(PiazzettaComponent *parent) { this->parent_ = parent; }
  void set_reglage(Reglage reglage) { this->reglage_ = reglage; }

 protected:
  void control(float value) override;
  PiazzettaComponent *parent_{nullptr};
  Reglage reglage_{Reglage::PUISSANCE};
};

// ENERGY SAVING : 0 = STOP (Off/+2/+4/+6), 1 = START (Off/-2/-4/-6) ; valeur = ecart en degres affiches, 0 = OFF
class PiazzettaSelect : public select::Select {
 public:
  void set_parent(PiazzettaComponent *parent) { this->parent_ = parent; }
  void set_genre(uint8_t g) { this->genre_ = g; }

 protected:
  void control(size_t index) override;
  PiazzettaComponent *parent_{nullptr};
  uint8_t genre_{0};
};

class PiazzettaButton : public button::Button {
 public:
  void set_parent(PiazzettaComponent *parent) { this->parent_ = parent; }
  void set_action(Action action) { this->action_ = action; }

 protected:
  void press_action() override;
  PiazzettaComponent *parent_{nullptr};
  Action action_{Action::INTERROGER};
};

class PiazzettaSwitch : public switch_::Switch {
 public:
  void set_parent(PiazzettaComponent *parent) { this->parent_ = parent; }
  void set_option(Option option) { this->option_ = option; }

 protected:
  void write_state(bool state) override;
  PiazzettaComponent *parent_{nullptr};
  Option option_{Option::EMISSION};
};

// Ecran envoye par le poele (32 octets) : B5 | bas gauche (4) | bas droite (5) | ligne 1 (9) | ligne 2 (9) | icones (3) | somme
struct Ecran {
  std::string ligne1, ligne2, bas;
  int heure_h{-1}, heure_m{-1};
  int temperature{-1000};
  int puissance{0};       // 1 a 4, 0 si inconnue
  int ventilateur{0};     // 1 a 4, 0 si inconnu
  bool menu{false};       // ecran « REGLER … »
  uint8_t menu_marque{0};  // 0x00 ventilateur, 0x50 puissance
  int menu_valeur{0};
  int consigne{0};        // SET ROOM TEMPERAT. : valeur affichee (F ou C), 0 si autre ecran
  int ens_ecran{0};       // ENERGY SAVING : 1 ecran STOP, 2 ecran START, 0 autre
  int ens_valeur{-1};     // ecart affiche (2/4/6), 0 = OFF, -1 illisible
  bool bas_vide{false};   // o[1..9] tout en espaces : listes de menus
};

class PiazzettaComponent : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_broches(int sck, int miso, int mosi, int cs, int rst, int dio0, int dio1, int dio2) {
    sck_ = sck; miso_ = miso; mosi_ = mosi; cs_ = cs; rst_ = rst; dio0_ = dio0; dio1_ = dio1; dio2_ = dio2;
  }
  void set_frequence(float f) { frequence_ = f; }
  void set_remote_id(uint8_t id) { remote_id_ = id; }
  void set_touche_interrogation(uint8_t t) { touche_interrogation_ = t; }
  void set_touche_maintien(uint8_t t) { touche_maintien_ = t; }
  // Appui long : touche et octet d'etat (bit 7) trame par trame ; vide = touche_maintien + ETATS_MAINTIEN (V3)
  void set_sequence_maintien(const std::vector<uint8_t> &touches, const std::vector<uint8_t> &etats) {
    touches_maintien_ = touches;
    etats_maintien_ = etats;
  }
  void set_periode_maintien(uint32_t ms) { periode_maintien_ms_ = ms; }
  // Reglages modifiables depuis HA (entites template de la YAML)
  void set_delai_touche(uint32_t ms) { delai_touche_ms_ = std::min<uint32_t>(ms, 1000); }
  bool regler_sequence_maintien(const std::string &texte);
  void appuyer_touches(const std::vector<int32_t> &touches, int ecart_ms, bool porte = false);
  void set_copier_etat(bool c) { copier_etat_ = c; }
  void set_ecoute_seule(bool e) { ecoute_seule_ = e; }
  void set_puissance_emission(int p) { puissance_emission_ = p; }
  void set_seuil_rssi(int s) { seuil_rssi_ = s; }
  void set_intervalle(uint32_t ms) { intervalle_ms_ = ms; }
  void set_temperature(sensor::Sensor *s) { temperature_ = s; }
  void set_rssi(sensor::Sensor *s) { rssi_ = s; }
  void set_phase(text_sensor::TextSensor *t) { phase_ = t; }
  void set_heure(text_sensor::TextSensor *t) { heure_ = t; }
  void set_etat(text_sensor::TextSensor *t) { etat_ = t; }
  void set_ligne1(text_sensor::TextSensor *t) { ligne1_ = t; }
  void set_ligne2(text_sensor::TextSensor *t) { ligne2_ = t; }
  void set_bas(text_sensor::TextSensor *t) { bas_ = t; }
  void set_derniere_trame(text_sensor::TextSensor *t) { derniere_trame_ = t; }
  void set_joignable(binary_sensor::BinarySensor *b) { joignable_ = b; }
  void set_puissance(PiazzettaNumber *n) { puissance_ = n; }
  void set_ventilateur(PiazzettaNumber *n) { ventilateur_ = n; }
  void set_consigne(PiazzettaNumber *n) { consigne_ = n; }
  void set_ens_stop(PiazzettaSelect *s) { ens_stop_ = s; }
  void set_ens_start(PiazzettaSelect *s) { ens_start_ = s; }
  // ENERGY SAVING STOP/START (ecart en degres affiches, 0 = OFF pour STOP, -1 = inchange)
  void demander_ens(int stop, int start);
  // Valeurs vues sur les ecrans SET (telecommande ou HA), pas le niveau module en marche
  void set_puissance_reglee(sensor::Sensor *s) { puissance_reglee_ = s; }
  void set_ventilateur_regle(sensor::Sensor *s) { ventilateur_regle_ = s; }
  void set_bascule_attente(binary_sensor::BinarySensor *b) { bascule_attente_ = b; }
  void set_age_ecran(sensor::Sensor *s) { age_ecran_ = s; }
  void set_trames_rejetees(sensor::Sensor *s) { trames_rejetees_ = s; }
  void set_trames_corrigees(sensor::Sensor *s) { trames_corrigees_ = s; }
  void set_temperature_telecommande(sensor::Sensor *s) { temperature_telecommande_ = s; }
  void set_fumees(sensor::Sensor *s) { fumees_ = s; }
  void set_rpm_fumees(sensor::Sensor *s) { rpm_fumees_ = s; }
  void set_vis(sensor::Sensor *s) { vis_ = s; }
  void set_emission(PiazzettaSwitch *s) { emission_ = s; }
  void set_synchro_auto(PiazzettaSwitch *s) { synchro_auto_sw_ = s; }

  void demander_reglage(Reglage reglage, int cible);
  void demander_action(Action action);
  // Appele par les interrupteurs HA ; publie le nouvel etat
  void regler_option(Option option, bool actif);
  // Pour l'ecran OLED
  bool emission_autorisee() const { return !this->ecoute_seule_; }

 protected:
  // --- Radio ---
  void configurer_radio_();
  void ecouter_();
  void emettre_octets_(std::vector<uint8_t> octets);
  void traiter_fronts_();
  void finir_trame_();
  void traiter_trame_(const std::vector<uint32_t> &durees, int rssi);
  void traiter_ecran_(const std::vector<uint8_t> &o);

  // --- Taches ---
  enum class Etape : uint8_t { REPOS, ATTENTE_MENU, ATTENTE_VALEUR, APPUI_LONG, SYNCHRO, SEQUENCE, ENS };
  void avancer_ens_(const Ecran &e, bool principal, bool reponse);
  void touche_ens_(uint8_t base);
  void avancer_tache_();
  void terminer_tache_(const char *bilan);
  void publier_bascule_();

  int sck_, miso_, mosi_, cs_, rst_, dio0_, dio1_, dio2_;
  float frequence_{433.92f};
  uint8_t remote_id_{0xC5};  // premier octet des trames de la telecommande
  uint8_t touche_interrogation_{0x00};
  uint8_t touche_maintien_{0x61};
  std::vector<uint8_t> touches_maintien_;
  std::vector<uint8_t> etats_maintien_;
  uint32_t periode_maintien_ms_{350};
  uint32_t delai_touche_ms_{150};
  // Emission differee (delai apres la derniere trame recue)
  uint32_t derniere_reception_ms_{0};
  std::vector<uint8_t> trame_planifiee_;
  bool trame_en_attente_{false};
  uint32_t emission_a_ms_{0};
  void planifier_(std::vector<uint8_t> octets);
  // Touches envoyees depuis HA
  std::vector<uint8_t> touches_seq_;
  uint32_t ecart_seq_ms_{400};
  bool porte_seq_{false};  // trappe ouverte : 3e octet 0x80
  // 2e octet emis : valeur fixe (F0203-1480V3), ou dernier octet entendu de la vraie telecommande
  // (MULTICOMFORT V5, ou c'est un compteur ; non verifie face au poele)
  bool copier_etat_{false};
  uint8_t etat_entendu_{0};  // 0 = rien entendu encore
  bool ecoute_seule_{false};  // aucune emission : sert de recepteur de surveillance
  bool synchro_auto_{true};   // synchronisation automatique (touche ventilateur) ; sans interrupteur : actif
  PiazzettaSwitch *emission_{nullptr};
  PiazzettaSwitch *synchro_auto_sw_{nullptr};
  // Copie la temperature ambiante (7 bits bas, en degres F) entendue ; le bit 7 (menu ouvert) reste celui de `fixe`
  uint8_t octet_etat_(uint8_t fixe) const {
    return this->copier_etat_ && this->etat_entendu_ != 0 ? (this->etat_entendu_ & 0x7F) | (fixe & 0x80) : fixe;
  }
  int puissance_emission_{10};
  int seuil_rssi_{-78};
  uint32_t intervalle_ms_{30000};

  sensor::Sensor *temperature_{nullptr};
  sensor::Sensor *rssi_{nullptr};
  text_sensor::TextSensor *phase_{nullptr};
  text_sensor::TextSensor *heure_{nullptr};
  text_sensor::TextSensor *etat_{nullptr};
  text_sensor::TextSensor *ligne1_{nullptr};
  text_sensor::TextSensor *ligne2_{nullptr};
  text_sensor::TextSensor *bas_{nullptr};
  text_sensor::TextSensor *derniere_trame_{nullptr};
  binary_sensor::BinarySensor *joignable_{nullptr};
  PiazzettaNumber *puissance_{nullptr};
  PiazzettaNumber *ventilateur_{nullptr};
  PiazzettaNumber *consigne_{nullptr};
  PiazzettaSelect *ens_stop_{nullptr};
  PiazzettaSelect *ens_start_{nullptr};
  sensor::Sensor *puissance_reglee_{nullptr};
  sensor::Sensor *ventilateur_regle_{nullptr};
  binary_sensor::BinarySensor *bascule_attente_{nullptr};
  sensor::Sensor *age_ecran_{nullptr};
  sensor::Sensor *trames_rejetees_{nullptr};
  sensor::Sensor *trames_corrigees_{nullptr};
  sensor::Sensor *temperature_telecommande_{nullptr};
  sensor::Sensor *fumees_{nullptr};      // STOVE STATUS : temperature des fumees
  sensor::Sensor *rpm_fumees_{nullptr};  // STOVE STATUS : extracteur, tr/min
  sensor::Sensor *vis_{nullptr};         // STOVE STATUS : vis sans fin, s

  HighFrequencyLoopRequester boucle_rapide_;

  // Trame en cours d'assemblage
  bool en_trame_{false};
  uint32_t debut_trame_{0}, dernier_front_{0}, dernier_fort_{0}, dernier_rssi_us_{0};
  uint8_t dernier_niveau_{0};
  std::vector<uint32_t> durees_;
  int nb_impulsions_{0};
  float rssi_max_{-200};

  // Etat connu
  uint32_t derniere_reponse_ms_{0};
  uint32_t derniere_telecommande_ms_{0};
  uint32_t derniere_interrogation_ms_{0};
  uint32_t dernier_joignable_ms_{0};
  bool ecran_recu_{false};
  bool ecran_principal_recu_{false};
  uint32_t prochaine_synchro_ms_{0};
  bool marche_arret_envoye_{false};
  uint32_t dernier_marche_arret_ms_{0};
  Ecran dernier_ecran_;
  std::string dernier_etat_;
  uint32_t dernier_ecran_principal_ms_{0};
  uint32_t demarrage_ms_{0};

  // Bascule marche/arret envoyee mais pas encore confirmee par un ecran
  bool bascule_en_attente_{false};
  std::string etat_avant_bascule_;

  // Diagnostics
  uint32_t nb_trames_rejetees_{0};
  uint32_t nb_trames_corrigees_{0};
  uint32_t dernier_ecran_rejete_ms_{0};  // 0 = aucun depuis le demarrage
  std::vector<uint8_t> dernier_b5_;      // dernier ecran complet valide (32 octets), pour y appliquer un B9
  uint32_t dernier_diagnostic_ms_{0};

  // Tache en cours (une seule commande radio a la fois)
  Etape etape_{Etape::REPOS};
  Reglage reglage_{Reglage::PUISSANCE};
  int cible_{0};
  int essais_{0};
  int pas_{0};
  int sens_{0};  // dernier pas envoye : +1, -1, 0 aucun
  uint32_t echeance_ms_{0};
  int derniere_consigne_{0};
  // Tache ENERGY SAVING
  int cible_ens_stop_{-1}, cible_ens_start_{-1};
  int ens_etape_{0};          // 0 ouverture, 1 dans le sous-menu, 2 START valide (SET envoye)
  int ens_appuis_{0};         // appuis + dans la liste du menu
  bool ens_attente_{false};   // une touche est partie, on attend son ecran
  std::vector<uint8_t> ens_trame_;  // derniere trame envoyee (renvoyee telle quelle si pas d'ecran)
  int ens_stop_vu_{-1}, ens_start_vu_{-1};
  // Le poele ignore une touche identique a la precedente (compteur compris) : compteur 1-3 en bits 5-6,
  // change a chaque touche de reglage comme la vraie telecommande (A8/C8/E8 ...). Constate 2026-11-20/23.
  // Pas toujours : le 2026-11-24 18:20, des renvois identiques de + ont agi (voir la marche Energy Saving).
  uint8_t compteur_touche_{0};
  uint8_t touche_(uint8_t base) {
    this->compteur_touche_ = this->compteur_touche_ % 3 + 1;
    return (base & 0x9F) | (this->compteur_touche_ << 5);
  }
  uint8_t touche_menu_() const;
  int valeur_reglage_(const Ecran &e) const;
  bool reponse_attendue_{false};
};

}  // namespace piazzetta
}  // namespace esphome
