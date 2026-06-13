// =============================================================================
//  airlines.h  -  ICAO callsign-prefix -> airline name lookup
//
//  ADS-B flights identify themselves with an ICAO callsign: a 3-letter airline
//  designator followed by a flight number (e.g. "RYR1JG" = Ryanair, "KLM65H" =
//  KLM, "BAW123" = British Airways). That prefix tells us the operating airline
//  instantly - no API call, no 404s - so we use it as the primary source for the
//  airline shown on the panel, falling back to the adsbdb "registered_owner".
//
//  General-aviation traffic squawks its registration instead (e.g. "GABCD",
//  "N12345"), which has no airline prefix; airlineFromCallsign() returns nullptr
//  for those so the caller can fall back or show nothing.
//
//  This is a curated, UK/Europe-weighted subset of the ~thousands of ICAO codes.
//  Add rows as needed - the table is searched linearly, but it's tiny.
// =============================================================================
#pragma once
#include <ctype.h>
#include <string.h>

struct AirlineCode { const char* icao; const char* name; };

static const AirlineCode AIRLINES[] = {
  // ---- UK & Ireland ----
  {"BAW", "British Airways"},   {"SHT", "British Airways"},   {"CFE", "BA CityFlyer"},
  {"EZY", "easyJet"},           {"EZS", "easyJet Switzerland"},{"EJU", "easyJet Europe"},
  {"RYR", "Ryanair"},           {"RUK", "Ryanair UK"},        {"RYS", "Ryanair"},
  {"EXS", "Jet2"},              {"TOM", "TUI Airways"},       {"VIR", "Virgin Atlantic"},
  {"LOG", "Loganair"},          {"WUK", "Wizz Air UK"},       {"EIN", "Aer Lingus"},
  {"ELB", "Aer Lingus UK"},     {"BMR", "BMI Regional"},      {"BCY", "CityJet"},
  {"TAY", "ASL Airlines Ireland"},{"ABR", "ASL Airlines Ireland"},{"NPT", "West Atlantic UK"},

  // ---- France / Benelux ----
  {"AFR", "Air France"},        {"KLM", "KLM"},               {"KLC", "KLM Cityhopper"},
  {"TVF", "Transavia France"},  {"TRA", "Transavia"},         {"HOP", "Air France Hop"},
  {"BEL", "Brussels Airlines"}, {"JAF", "TUI fly Belgium"},   {"TFL", "TUI fly Netherlands"},
  {"LGL", "Luxair"},

  // ---- Germany / Switzerland / Austria ----
  {"DLH", "Lufthansa"},         {"CLH", "Lufthansa CityLine"},{"GEC", "Lufthansa Cargo"},
  {"OCN", "Discover Airlines"}, {"EWG", "Eurowings"},         {"CFG", "Condor"},
  {"TUI", "TUI fly"},
  {"SWR", "Swiss"},             {"EDW", "Edelweiss Air"},     {"AUA", "Austrian"},
  {"LDM", "Lauda Europe"},

  // ---- Iberia / Italy ----
  {"IBE", "Iberia"},            {"IBS", "Iberia Express"},    {"ANE", "Air Nostrum"},
  {"VLG", "Vueling"},           {"AEA", "Air Europa"},        {"VOE", "Volotea"},
  {"TAP", "TAP Air Portugal"},  {"ITY", "ITA Airways"},       {"NOS", "Neos"},

  // ---- Nordic / Baltic ----
  {"SAS", "SAS"},               {"FIN", "Finnair"},           {"NAX", "Norwegian"},
  {"NOZ", "Norwegian"},         {"NSZ", "Norwegian"},         {"IBK", "Norwegian"},
  {"BLX", "TUI fly Nordic"},    {"WIF", "Wideroe"},           {"ICE", "Icelandair"},
  {"FLI", "Atlantic Airways"},  {"BTI", "airBaltic"},         {"EST", "Estonian (Nordica)"},

  // ---- Central & Eastern Europe ----
  {"WZZ", "Wizz Air"},          {"WMT", "Wizz Air Malta"},    {"LOT", "LOT Polish"},
  {"CSA", "Czech Airlines"},    {"TVS", "Smartwings"},        {"CTN", "Croatia Airlines"},
  {"AEE", "Aegean"},            {"OAL", "Olympic Air"},       {"ROT", "Tarom"},
  {"BUC", "Bulgaria Air"},      {"AUI", "Ukraine Intl"},      {"JKK", "Air Serbia"},
  {"PGT", "Pegasus"},           {"THY", "Turkish Airlines"},  {"SXS", "SunExpress"},
  {"KZR", "Air Astana"},

  // ---- Russia ----
  {"AFL", "Aeroflot"},          {"SDM", "Rossiya"},           {"SBI", "S7 Airlines"},
  {"UTA", "UTair"},             {"PBD", "Pobeda"},            {"SVR", "Ural Airlines"},

  // ---- Middle East ----
  {"UAE", "Emirates"},          {"QTR", "Qatar Airways"},     {"ETD", "Etihad"},
  {"FDB", "flydubai"},          {"ABY", "Air Arabia"},        {"GFA", "Gulf Air"},
  {"OMA", "Oman Air"},          {"SVA", "Saudia"},            {"MEA", "Middle East Airlines"},
  {"RJA", "Royal Jordanian"},   {"ELY", "El Al"},             {"KAC", "Kuwait Airways"},
  {"JZR", "Jazeera"},           {"IRA", "Iran Air"},

  // ---- Africa ----
  {"MSR", "EgyptAir"},              {"RAM", "Royal Air Maroc"},   {"DAH", "Air Algerie"},
  {"TAR", "Tunisair"},              {"ETH", "Ethiopian"},         {"KQA", "Kenya Airways"},
  {"SAA", "South African Airways"}, {"RWD", "RwandAir"},          {"MAU", "Air Mauritius"},

  // ---- South & East Asia ----
  {"AIC", "Air India"},         {"IGO", "IndiGo"},            {"VTI", "Vistara"},
  {"SEJ", "SpiceJet"},          {"AKJ", "Akasa Air"},         {"THA", "Thai Airways"},
  {"TVJ", "Thai Vietjet"},      {"VJC", "VietJet Air"},       {"HVN", "Vietnam Airlines"},
  {"BKP", "Bangkok Airways"},   {"MAS", "Malaysia Airlines"}, {"AXM", "AirAsia"},
  {"SIA", "Singapore Airlines"},{"SLK", "SilkAir"},           {"TGW", "Scoot"},
  {"GIA", "Garuda Indonesia"},  {"LNI", "Lion Air"},          {"PAL", "Philippine Airlines"},
  {"CEB", "Cebu Pacific"},      {"CES", "China Eastern"},     {"CSN", "China Southern"},
  {"CCA", "Air China"},         {"CHH", "Hainan Airlines"},   {"CSZ", "Shenzhen Airlines"},
  {"CXA", "Xiamen Air"},        {"CQH", "Spring Airlines"},   {"CPA", "Cathay Pacific"},
  {"HKE", "Hong Kong Express"}, {"EVA", "EVA Air"},           {"CAL", "China Airlines"},
  {"ANA", "All Nippon"},        {"JAL", "Japan Airlines"},    {"APJ", "Peach Aviation"},
  {"KAL", "Korean Air"},        {"AAR", "Asiana"},            {"JJA", "Jeju Air"},
  {"TWB", "Tway Air"},

  // ---- Oceania ----
  {"QFA", "Qantas"},            {"JST", "Jetstar"},           {"VOZ", "Virgin Australia"},
  {"ANZ", "Air New Zealand"},

  // ---- North America ----
  {"DAL", "Delta"},             {"UAL", "United Airlines"},   {"AAL", "American Airlines"},
  {"SWA", "Southwest"},         {"JBU", "JetBlue"},           {"ASA", "Alaska Airlines"},
  {"NKS", "Spirit"},            {"FFT", "Frontier"},          {"HAL", "Hawaiian Airlines"},
  {"ACA", "Air Canada"},        {"ROU", "Air Canada Rouge"},  {"JZA", "Air Canada Jazz"},
  {"WJA", "WestJet"},           {"TSC", "Air Transat"},       {"POE", "Porter Airlines"},
  {"AMX", "Aeromexico"},        {"VOI", "Volaris"},           {"VIV", "Viva Aerobus"},

  // ---- Latin America ----
  {"LAN", "LATAM"},             {"TAM", "LATAM Brasil"},      {"GLO", "Gol"},
  {"AZU", "Azul"},              {"ARG", "Aerolineas Argentinas"},{"AVA", "Avianca"},
  {"CMP", "Copa Airlines"},

  // ---- Cargo / express ----
  {"FDX", "FedEx"},             {"UPS", "UPS"},               {"GTI", "Atlas Air"},
  {"CLX", "Cargolux"},          {"ABW", "AirBridgeCargo"},    {"BOX", "AeroLogic"},
  {"BCS", "DHL Air (EAT)"},     {"CKS", "Kalitta Air"},       {"ABX", "ABX Air"},
  {"GSS", "Atlas Air (Global)"},{"QAC", "Qatar Cargo"},
};

// Resolve the airline name from an ADS-B callsign, or nullptr if the callsign is
// not a recognised 3-letter ICAO airline code followed by a flight number.
static inline const char* airlineFromCallsign(const char* cs) {
  if (!cs) return nullptr;
  // Need three leading A-Z letters, then a digit (the flight number). This skips
  // GA registrations like "GABCD" (4th char is a letter) and "N12345" (digit 2nd).
  if (!(cs[0] >= 'A' && cs[0] <= 'Z' &&
        cs[1] >= 'A' && cs[1] <= 'Z' &&
        cs[2] >= 'A' && cs[2] <= 'Z')) return nullptr;
  if (!isdigit((unsigned char)cs[3])) return nullptr;
  for (const AirlineCode& a : AIRLINES)
    if (a.icao[0] == cs[0] && a.icao[1] == cs[1] && a.icao[2] == cs[2] && a.icao[3] == '\0')
      return a.name;
  return nullptr;
}
