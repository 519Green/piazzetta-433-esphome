# ESPHome external component for Piazzetta pellet stoves driven by the 433 MHz F0203-1480V3 remote.
# Protocol: see docs/protocol.md. Code comments are in French (original project language).
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, button, number, select, sensor, switch, text_sensor
from esphome.const import (
    CONF_ID,
    DEVICE_CLASS_CONNECTIVITY,
    DEVICE_CLASS_DURATION,
    DEVICE_CLASS_SIGNAL_STRENGTH,
    DEVICE_CLASS_TEMPERATURE,
    ENTITY_CATEGORY_CONFIG,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_CELSIUS,
    UNIT_DECIBEL_MILLIWATT,
    UNIT_SECOND,
)

CODEOWNERS = ["@MaximeHTL"]
AUTO_LOAD = ["sensor", "text_sensor", "binary_sensor", "number", "button", "switch", "select"]

piazzetta_ns = cg.esphome_ns.namespace("piazzetta")
PiazzettaComponent = piazzetta_ns.class_("PiazzettaComponent", cg.Component)
PiazzettaNumber = piazzetta_ns.class_("PiazzettaNumber", number.Number)
PiazzettaButton = piazzetta_ns.class_("PiazzettaButton", button.Button)
PiazzettaSwitch = piazzetta_ns.class_("PiazzettaSwitch", switch.Switch)
PiazzettaSelect = piazzetta_ns.class_("PiazzettaSelect", select.Select)
Reglage = piazzetta_ns.enum("Reglage", is_class=True)
Action = piazzetta_ns.enum("Action", is_class=True)
Option = piazzetta_ns.enum("Option", is_class=True)

CONF_PIN_SCK = "pin_sck"
CONF_PIN_MISO = "pin_miso"
CONF_PIN_MOSI = "pin_mosi"
CONF_PIN_CS = "pin_cs"
CONF_PIN_RST = "pin_rst"
CONF_PIN_DIO0 = "pin_dio0"
CONF_PIN_DIO1 = "pin_dio1"
CONF_PIN_DIO2 = "pin_dio2"
CONF_FREQUENCE = "frequence"
CONF_REMOTE_ID = "remote_id"
CONF_TOUCHE_INTERROGATION = "touche_interrogation"
CONF_TOUCHE_MAINTIEN = "touche_maintien"
CONF_SEQUENCE_MAINTIEN = "sequence_maintien"
CONF_TOUCHE = "touche"
CONF_ETAT_OCTET = "etat"
CONF_PERIODE_MAINTIEN = "periode_maintien"
CONF_COPIER_OCTET_ETAT = "copier_octet_etat"
CONF_ECOUTE_SEULE = "ecoute_seule"
CONF_DERNIERE_TRAME = "derniere_trame"
CONF_TOUCHE_TELECOMMANDE = "touche_telecommande"
CONF_PUISSANCE_EMISSION = "puissance_emission"
CONF_SEUIL_RSSI = "seuil_rssi"
CONF_INTERVALLE = "intervalle_interrogation"
CONF_TEMPERATURE = "temperature"
CONF_RSSI = "rssi"
CONF_PHASE = "phase"
CONF_HEURE = "heure"
CONF_ETAT = "etat"
CONF_LIGNE1 = "ecran_ligne1"
CONF_LIGNE2 = "ecran_ligne2"
CONF_BAS = "ecran_bas"
CONF_JOIGNABLE = "joignable"
CONF_PUISSANCE = "puissance"
CONF_VENTILATEUR = "ventilateur"
CONF_CONSIGNE = "consigne"
CONF_ENS_STOP = "ens_stop"
CONF_ENS_START = "ens_start"
CONF_PUISSANCE_REGLEE = "puissance_reglee"
CONF_VENTILATEUR_REGLE = "ventilateur_regle"
CONF_INTERROGER = "interroger"
CONF_MARCHE_ARRET = "marche_arret"
CONF_BASCULE_ATTENTE = "bascule_en_attente"
CONF_AGE_ECRAN = "age_ecran"
CONF_TRAMES_REJETEES = "trames_rejetees"
CONF_TRAMES_CORRIGEES = "trames_corrigees"
CONF_TEMPERATURE_TELECOMMANDE = "temperature_telecommande"
CONF_FUMEES = "temperature_fumees"
CONF_RPM_FUMEES = "rpm_fumees"
CONF_VIS = "vis"
CONF_EMISSION = "emission"
CONF_SYNCHRO_AUTO = "synchro_auto"

broche = cv.int_range(min=0, max=39)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(PiazzettaComponent),
        # Brochage du banc de test (ESP32 DevKit + Ra-02)
        cv.Optional(CONF_PIN_SCK, default=18): broche,
        cv.Optional(CONF_PIN_MISO, default=19): broche,
        cv.Optional(CONF_PIN_MOSI, default=23): broche,
        cv.Optional(CONF_PIN_CS, default=5): broche,
        cv.Optional(CONF_PIN_RST, default=14): broche,
        cv.Optional(CONF_PIN_DIO0, default=26): broche,
        cv.Optional(CONF_PIN_DIO1, default=33): broche,
        cv.Optional(CONF_PIN_DIO2, default=32): broche,
        cv.Optional(CONF_FREQUENCE, default=433.92): cv.float_range(min=410, max=525),
        # First byte of every remote frame (0xC5 on the reference installation). It may be specific to
        # each remote/stove pairing: capture yours with the test bench (see docs/capture-guide.md).
        cv.Optional(CONF_REMOTE_ID, default=0xC5): cv.hex_uint8_t,
        # Key codes that differ between remote generations. Defaults: F0203-1480V3.
        # MULTICOMFORT V5 (F02034130): poll 0x80, power held 0xA1.
        cv.Optional(CONF_TOUCHE_INTERROGATION, default=0x00): cv.hex_uint8_t,
        cv.Optional(CONF_TOUCHE_MAINTIEN, default=0x61): cv.hex_uint8_t,
        # Long press, frame by frame after the 0x81 press: key byte and byte-2 template (only bit 7 is used
        # when copier_octet_etat is on). Overrides touche_maintien. V5 capture 2026-11-19: 21 21 41 61 61 ...
        cv.Optional(CONF_SEQUENCE_MAINTIEN): cv.All(
            cv.ensure_list(
                cv.Schema({cv.Required(CONF_TOUCHE): cv.hex_uint8_t, cv.Optional(CONF_ETAT_OCTET, default=0x3E): cv.hex_uint8_t})
            ),
            cv.Length(min=1, max=30),
        ),
        # Gap between long-press frames; the V5 remote waits ~430 ms (the stove answers each frame with a full screen)
        cv.Optional(CONF_PERIODE_MAINTIEN, default="350ms"): cv.positive_time_period_milliseconds,
        # Second byte of transmitted frames: fixed V3 values (false), or a copy of the last value heard
        # from the real remote (true; on the V5 it is a counter). V5 behaviour not yet verified on a stove.
        cv.Optional(CONF_COPIER_OCTET_ETAT, default=False): cv.boolean,
        # Listen only: never transmit (polls, resync and commands are logged as blocked).
        cv.Optional(CONF_ECOUTE_SEULE, default=False): cv.boolean,
        cv.Optional(CONF_PUISSANCE_EMISSION, default=10): cv.int_range(min=2, max=17),
        cv.Optional(CONF_SEUIL_RSSI, default=-78): cv.int_range(min=-120, max=-30),
        cv.Optional(CONF_INTERVALLE, default="30s"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_TEMPERATURE): sensor.sensor_schema(
            unit_of_measurement=UNIT_CELSIUS,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_TEMPERATURE,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_RSSI): sensor.sensor_schema(
            unit_of_measurement=UNIT_DECIBEL_MILLIWATT,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_SIGNAL_STRENGTH,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_PHASE): text_sensor.text_sensor_schema(icon="mdi:fireplace"),
        cv.Optional(CONF_ETAT): text_sensor.text_sensor_schema(icon="mdi:fire-circle"),
        cv.Optional(CONF_LIGNE1): text_sensor.text_sensor_schema(icon="mdi:remote"),
        cv.Optional(CONF_LIGNE2): text_sensor.text_sensor_schema(icon="mdi:remote"),
        cv.Optional(CONF_BAS): text_sensor.text_sensor_schema(icon="mdi:remote"),
        # Last valid frame heard (remote or stove), hex + RSSI
        cv.Optional(CONF_DERNIERE_TRAME): text_sensor.text_sensor_schema(
            icon="mdi:radio-tower", entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        ),
        cv.Optional(CONF_HEURE): text_sensor.text_sensor_schema(
            icon="mdi:clock-outline", entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        ),
        # Key pressed on the real remote (decoded name, one value per press; polls ignored)
        cv.Optional(CONF_TOUCHE_TELECOMMANDE): text_sensor.text_sensor_schema(icon="mdi:remote"),
        cv.Optional(CONF_JOIGNABLE): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_CONNECTIVITY,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        # Diagnostics : bascule non confirmee, fraicheur de l'ecran, qualite de reception
        cv.Optional(CONF_BASCULE_ATTENTE): binary_sensor.binary_sensor_schema(
            icon="mdi:timer-sand",
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_AGE_ECRAN): sensor.sensor_schema(
            unit_of_measurement=UNIT_SECOND,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_DURATION,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_TRAMES_REJETEES): sensor.sensor_schema(
            icon="mdi:alert-circle-outline",
            accuracy_decimals=0,
            state_class=STATE_CLASS_TOTAL_INCREASING,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        # Raw low 7 bits of byte 2 of the real remote's frames (assumed room temperature; unverified)
        cv.Optional(CONF_TEMPERATURE_TELECOMMANDE): sensor.sensor_schema(
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            icon="mdi:remote",
        ),
        # STOVE STATUS screen (read when HA opens it): exhaust temperature, exhaust fan rpm, auger seconds
        cv.Optional(CONF_FUMEES): sensor.sensor_schema(accuracy_decimals=0, device_class=DEVICE_CLASS_TEMPERATURE, state_class=STATE_CLASS_MEASUREMENT),
        cv.Optional(CONF_RPM_FUMEES): sensor.sensor_schema(unit_of_measurement="rpm", accuracy_decimals=0, icon="mdi:fan", state_class=STATE_CLASS_MEASUREMENT),
        cv.Optional(CONF_VIS): sensor.sensor_schema(unit_of_measurement=UNIT_SECOND, accuracy_decimals=1, icon="mdi:screw-lag", state_class=STATE_CLASS_MEASUREMENT),
        cv.Optional(CONF_TRAMES_CORRIGEES): sensor.sensor_schema(
            icon="mdi:auto-fix",
            accuracy_decimals=0,
            state_class=STATE_CLASS_TOTAL_INCREASING,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_PUISSANCE): number.number_schema(PiazzettaNumber, icon="mdi:fire"),
        cv.Optional(CONF_VENTILATEUR): number.number_schema(PiazzettaNumber, icon="mdi:fan"),
        # Room setpoint (SET ROOM TEMPERAT.), in the unit the stove shows; 1 C steps (66/68/70 F ...)
        cv.Optional(CONF_CONSIGNE): number.number_schema(PiazzettaNumber, icon="mdi:thermostat"),
        # ENERGY SAVING STOP / START (degrees shown, F on this stove); state read from the menu screens.
        # START Off is shown but refused as a command (the stove would never relight). Options carry " F" so HA
        # templates never parse "+4" into the number 4.
        cv.Optional(CONF_ENS_STOP): select.select_schema(PiazzettaSelect, icon="mdi:thermometer-chevron-up"),
        cv.Optional(CONF_ENS_START): select.select_schema(PiazzettaSelect, icon="mdi:thermometer-chevron-down"),
        # Set power / fan as shown on the SET screens (remote or HA), not the live modulated level
        cv.Optional(CONF_PUISSANCE_REGLEE): sensor.sensor_schema(accuracy_decimals=0, icon="mdi:fire"),
        cv.Optional(CONF_VENTILATEUR_REGLE): sensor.sensor_schema(accuracy_decimals=0, icon="mdi:fan"),
        cv.Optional(CONF_INTERROGER): button.button_schema(PiazzettaButton, icon="mdi:refresh"),
        cv.Optional(CONF_MARCHE_ARRET): button.button_schema(PiazzettaButton, icon="mdi:power"),
        # Runtime switches. ALWAYS_OFF by default: every reboot comes back listen-only / without auto sync.
        # Transmit overrides ecoute_seule at boot; Auto sync = automatic fan/menu key press to get a full screen.
        cv.Optional(CONF_EMISSION): switch.switch_schema(
            PiazzettaSwitch, icon="mdi:radio-tower", entity_category=ENTITY_CATEGORY_CONFIG, default_restore_mode="ALWAYS_OFF"
        ),
        cv.Optional(CONF_SYNCHRO_AUTO): switch.switch_schema(
            PiazzettaSwitch, icon="mdi:sync", entity_category=ENTITY_CATEGORY_CONFIG, default_restore_mode="ALWAYS_OFF"
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    cg.add_library("SPI", None)
    cg.add_library("jgromes/RadioLib", "7.8.1")

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(
        var.set_broches(
            config[CONF_PIN_SCK],
            config[CONF_PIN_MISO],
            config[CONF_PIN_MOSI],
            config[CONF_PIN_CS],
            config[CONF_PIN_RST],
            config[CONF_PIN_DIO0],
            config[CONF_PIN_DIO1],
            config[CONF_PIN_DIO2],
        )
    )
    cg.add(var.set_frequence(config[CONF_FREQUENCE]))
    cg.add(var.set_remote_id(config[CONF_REMOTE_ID]))
    cg.add(var.set_touche_interrogation(config[CONF_TOUCHE_INTERROGATION]))
    cg.add(var.set_touche_maintien(config[CONF_TOUCHE_MAINTIEN]))
    if CONF_SEQUENCE_MAINTIEN in config:
        seq = config[CONF_SEQUENCE_MAINTIEN]
        cg.add(var.set_sequence_maintien([t[CONF_TOUCHE] for t in seq], [t[CONF_ETAT_OCTET] for t in seq]))
    cg.add(var.set_periode_maintien(config[CONF_PERIODE_MAINTIEN]))
    cg.add(var.set_copier_etat(config[CONF_COPIER_OCTET_ETAT]))
    cg.add(var.set_ecoute_seule(config[CONF_ECOUTE_SEULE]))
    cg.add(var.set_puissance_emission(config[CONF_PUISSANCE_EMISSION]))
    cg.add(var.set_seuil_rssi(config[CONF_SEUIL_RSSI]))
    cg.add(var.set_intervalle(config[CONF_INTERVALLE]))

    if CONF_TEMPERATURE in config:
        cg.add(var.set_temperature(await sensor.new_sensor(config[CONF_TEMPERATURE])))
    if CONF_RSSI in config:
        cg.add(var.set_rssi(await sensor.new_sensor(config[CONF_RSSI])))
    if CONF_PHASE in config:
        cg.add(var.set_phase(await text_sensor.new_text_sensor(config[CONF_PHASE])))
    for cle, setter in (
        (CONF_ETAT, "set_etat"),
        (CONF_LIGNE1, "set_ligne1"),
        (CONF_LIGNE2, "set_ligne2"),
        (CONF_BAS, "set_bas"),
        (CONF_DERNIERE_TRAME, "set_derniere_trame"),
        (CONF_TOUCHE_TELECOMMANDE, "set_touche_telecommande"),
    ):
        if cle in config:
            cg.add(getattr(var, setter)(await text_sensor.new_text_sensor(config[cle])))
    if CONF_HEURE in config:
        cg.add(var.set_heure(await text_sensor.new_text_sensor(config[CONF_HEURE])))
    if CONF_JOIGNABLE in config:
        cg.add(var.set_joignable(await binary_sensor.new_binary_sensor(config[CONF_JOIGNABLE])))
    if CONF_BASCULE_ATTENTE in config:
        cg.add(var.set_bascule_attente(await binary_sensor.new_binary_sensor(config[CONF_BASCULE_ATTENTE])))
    for cle, setter in (
        (CONF_AGE_ECRAN, "set_age_ecran"),
        (CONF_TRAMES_REJETEES, "set_trames_rejetees"),
        (CONF_TRAMES_CORRIGEES, "set_trames_corrigees"),
        (CONF_TEMPERATURE_TELECOMMANDE, "set_temperature_telecommande"),
        (CONF_FUMEES, "set_fumees"),
        (CONF_RPM_FUMEES, "set_rpm_fumees"),
        (CONF_VIS, "set_vis"),
        (CONF_PUISSANCE_REGLEE, "set_puissance_reglee"),
        (CONF_VENTILATEUR_REGLE, "set_ventilateur_regle"),
    ):
        if cle in config:
            cg.add(getattr(var, setter)(await sensor.new_sensor(config[cle])))

    for cle, reglage, setter in (
        (CONF_PUISSANCE, Reglage.PUISSANCE, "set_puissance"),
        (CONF_VENTILATEUR, Reglage.VENTILATEUR, "set_ventilateur"),
    ):
        if cle in config:
            # Power P1-P5 (the P963 shows P5); fan 1-4
            n = await number.new_number(config[cle], min_value=1, max_value=5 if cle == CONF_PUISSANCE else 4, step=1)
            cg.add(n.set_parent(var))
            cg.add(n.set_reglage(reglage))
            cg.add(getattr(var, setter)(n))
    if CONF_CONSIGNE in config:
        # Stove range 7-30 C = 45-86 F
        n = await number.new_number(config[CONF_CONSIGNE], min_value=7, max_value=86, step=1)
        cg.add(n.set_parent(var))
        cg.add(n.set_reglage(Reglage.CONSIGNE))
        cg.add(var.set_consigne(n))
    for cle, genre, options, setter in (
        (CONF_ENS_STOP, 0, ["Off", "+2 F", "+4 F", "+6 F"], "set_ens_stop"),
        (CONF_ENS_START, 1, ["Off", "-2 F", "-4 F", "-6 F"], "set_ens_start"),
    ):
        if cle in config:
            sel = await select.new_select(config[cle], options=options)
            cg.add(sel.set_parent(var))
            cg.add(sel.set_genre(genre))
            cg.add(getattr(var, setter)(sel))

    for cle, action in (
        (CONF_INTERROGER, Action.INTERROGER),
        (CONF_MARCHE_ARRET, Action.MARCHE_ARRET),
    ):
        if cle in config:
            b = await button.new_button(config[cle])
            cg.add(b.set_parent(var))
            cg.add(b.set_action(action))

    for cle, option, setter in (
        (CONF_EMISSION, Option.EMISSION, "set_emission"),
        (CONF_SYNCHRO_AUTO, Option.SYNCHRO_AUTO, "set_synchro_auto"),
    ):
        if cle in config:
            sw = await switch.new_switch(config[cle])
            cg.add(sw.set_parent(var))
            cg.add(sw.set_option(option))
            cg.add(getattr(var, setter)(sw))
