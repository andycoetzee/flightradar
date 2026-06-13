// =============================================================================
//  aircraft_types.h  -  ICAO type designator -> full aircraft name lookup
//
//  ADS-B feeds tag each aircraft with a short ICAO type designator in the "t"
//  field (e.g. "B738" = Boeing 737-800, "A20N" = Airbus A320neo, "E190" =
//  Embraer 190). That code is already in the feed, so we can expand it to a
//  readable name instantly - no API call, no 404s - exactly like airlines.h
//  does for the operator. We use this as the primary source for the full type
//  shown on the panel, falling back to the adsbdb lookup only for codes not in
//  this table.
//
//  This is a broad-but-not-exhaustive set, UK/Europe-weighted toward airline
//  and common GA/business types. Add rows as needed - it's searched linearly,
//  but it's small and runs only when the selection/nearest changes.
// =============================================================================
#pragma once
#include <string.h>

struct AircraftType { const char* icao; const char* name; };

static const AircraftType AIRCRAFT_TYPES[] = {
  // ---- Airbus narrowbody ----
  {"A19N", "Airbus A319neo"},   {"A20N", "Airbus A320neo"},   {"A21N", "Airbus A321neo"},
  {"A318", "Airbus A318"},      {"A319", "Airbus A319"},      {"A320", "Airbus A320"},
  {"A321", "Airbus A321"},      {"BCS1", "Airbus A220-100"},  {"BCS3", "Airbus A220-300"},
  // ---- Airbus widebody ----
  {"A306", "Airbus A300-600"},  {"A30B", "Airbus A300"},      {"A310", "Airbus A310"},
  {"A332", "Airbus A330-200"},  {"A333", "Airbus A330-300"},  {"A338", "Airbus A330-800neo"},
  {"A339", "Airbus A330-900neo"},{"A342", "Airbus A340-200"}, {"A343", "Airbus A340-300"},
  {"A345", "Airbus A340-500"},  {"A346", "Airbus A340-600"},  {"A359", "Airbus A350-900"},
  {"A35K", "Airbus A350-1000"}, {"A388", "Airbus A380-800"},  {"A400", "Airbus A400M"},

  // ---- Boeing 737 family ----
  {"B712", "Boeing 717"},       {"B722", "Boeing 727-200"},
  {"B732", "Boeing 737-200"},   {"B733", "Boeing 737-300"},   {"B734", "Boeing 737-400"},
  {"B735", "Boeing 737-500"},   {"B736", "Boeing 737-600"},   {"B737", "Boeing 737-700"},
  {"B738", "Boeing 737-800"},   {"B739", "Boeing 737-900"},   {"B37M", "Boeing 737 MAX 7"},
  {"B38M", "Boeing 737 MAX 8"}, {"B39M", "Boeing 737 MAX 9"}, {"B3XM", "Boeing 737 MAX 10"},
  // ---- Boeing 747 / 757 / 767 ----
  {"B741", "Boeing 747-100"},   {"B742", "Boeing 747-200"},   {"B743", "Boeing 747-300"},
  {"B744", "Boeing 747-400"},   {"B748", "Boeing 747-8"},     {"B74S", "Boeing 747SP"},
  {"B752", "Boeing 757-200"},   {"B753", "Boeing 757-300"},   {"B762", "Boeing 767-200"},
  {"B763", "Boeing 767-300"},   {"B764", "Boeing 767-400"},
  // ---- Boeing 777 / 787 ----
  {"B772", "Boeing 777-200"},   {"B77L", "Boeing 777-200LR"}, {"B773", "Boeing 777-300"},
  {"B77W", "Boeing 777-300ER"}, {"B778", "Boeing 777-8"},     {"B779", "Boeing 777-9"},
  {"B788", "Boeing 787-8"},     {"B789", "Boeing 787-9"},     {"B78X", "Boeing 787-10"},

  // ---- Embraer ----
  {"E110", "Embraer EMB 110"},  {"E120", "Embraer EMB 120"},  {"E135", "Embraer ERJ-135"},
  {"E145", "Embraer ERJ-145"},  {"E170", "Embraer 170"},      {"E75S", "Embraer 175"},
  {"E75L", "Embraer 175"},      {"E190", "Embraer 190"},      {"E195", "Embraer 195"},
  {"E290", "Embraer E190-E2"},  {"E295", "Embraer E195-E2"},

  // ---- Bombardier / Canadair / De Havilland ----
  {"CRJ1", "Bombardier CRJ100"},{"CRJ2", "Bombardier CRJ200"},{"CRJ7", "Bombardier CRJ700"},
  {"CRJ9", "Bombardier CRJ900"},{"CRJX", "Bombardier CRJ1000"},
  {"DH8A", "Dash 8-100"},       {"DH8B", "Dash 8-200"},       {"DH8C", "Dash 8-300"},
  {"DH8D", "Dash 8-400"},

  // ---- ATR / regional turboprop ----
  {"AT43", "ATR 42-300"},       {"AT45", "ATR 42-500"},       {"AT46", "ATR 42-600"},
  {"AT72", "ATR 72"},           {"AT75", "ATR 72-500"},       {"AT76", "ATR 72-600"},
  {"SF34", "Saab 340"},         {"SB20", "Saab 2000"},        {"JS41", "Jetstream 41"},

  // ---- Fokker / BAe / Avro ----
  {"F70",  "Fokker 70"},        {"F100", "Fokker 100"},       {"RJ85", "Avro RJ85"},
  {"RJ1H", "Avro RJ100"},       {"B461", "BAe 146-100"},      {"B462", "BAe 146-200"},
  {"B463", "BAe 146-300"},

  // ---- General aviation / light ----
  {"C152", "Cessna 152"},       {"C172", "Cessna 172"},       {"C182", "Cessna 182"},
  {"C208", "Cessna 208 Caravan"},{"C210", "Cessna 210"},      {"P28A", "Piper PA-28"},
  {"PA28", "Piper PA-28"},      {"PA34", "Piper Seneca"},     {"PA46", "Piper Malibu"},
  {"SR20", "Cirrus SR20"},      {"SR22", "Cirrus SR22"},      {"DA40", "Diamond DA40"},
  {"DA42", "Diamond DA42"},     {"TBM9", "Daher TBM 900"},    {"PC12", "Pilatus PC-12"},
  {"BE20", "King Air 200"},     {"B350", "King Air 350"},

  // ---- Business jets ----
  {"C25A", "Citation CJ2"},     {"C25B", "Citation CJ3"},     {"C56X", "Citation Excel"},
  {"E55P", "Phenom 300"},       {"E50P", "Phenom 100"},       {"LJ45", "Learjet 45"},
  {"LJ60", "Learjet 60"},       {"GLF5", "Gulfstream V"},     {"GLF6", "Gulfstream G650"},
  {"GLEX", "Global Express"},   {"CL35", "Challenger 350"},   {"CL60", "Challenger 600"},
  {"FA7X", "Falcon 7X"},        {"F2TH", "Falcon 2000"},      {"H25B", "Hawker 800"},
  {"PC24", "Pilatus PC-24"},

  // ---- Helicopters ----
  {"EC35", "Airbus H135"},      {"EC45", "Airbus H145"},      {"EC30", "Airbus H130"},
  {"A109", "AgustaWestland AW109"},{"A139", "AgustaWestland AW139"},{"B06", "Bell 206"},
  {"B407", "Bell 407"},
  {"R44",  "Robinson R44"},     {"R66",  "Robinson R66"},     {"S76",  "Sikorsky S-76"},

  // ---- Military fast jets (common over the UK) ----
  {"EUFI", "Eurofighter Typhoon"},{"F35",  "F-35 Lightning II"},{"TOR",  "Panavia Tornado"},
  {"F15",  "F-15 Eagle"},        {"F16",  "F-16 Fighting Falcon"},{"F18",  "F/A-18 Hornet"},
  {"A10",  "A-10 Thunderbolt II"},{"HAWK", "BAE Hawk"},          {"TEX2", "T-6 Texan II"},
  {"G115", "Grob Tutor"},        {"GROB", "Grob Tutor"},        {"L159", "Aero L-159 ALCA"},

  // ---- Military transport / tanker / ISR ----
  {"C130", "Lockheed C-130 Hercules"},{"C30J", "Lockheed C-130J"},{"C17",  "C-17 Globemaster III"},
  {"C5M",  "C-5M Super Galaxy"}, {"K35R", "Boeing KC-135"},     {"K46",  "Boeing KC-46 Pegasus"},
  {"P8",   "P-8 Poseidon"},      {"R135", "RC-135 Rivet Joint"},{"E3TF", "Boeing E-3 Sentry"},
  {"B52",  "B-52 Stratofortress"},{"U2",  "U-2 Dragon Lady"},   {"C295", "Airbus C295"},
  {"A124", "Antonov An-124"},

  // ---- Military helicopters / tiltrotor ----
  {"H47",  "CH-47 Chinook"},     {"H64",  "AH-64 Apache"},      {"EH10", "AW101 Merlin"},
  {"A159", "AW159 Wildcat"},     {"PUMA", "SA330 Puma"},        {"V22",  "V-22 Osprey"},
  {"H60",  "UH-60 Black Hawk"},
};

// Resolve a full aircraft name from the feed's short ICAO type designator, or
// nullptr if the code isn't in the table (caller can fall back to adsbdb / the
// short code itself).
static inline const char* typeNameFromIcao(const char* icao) {
  if (!icao || !icao[0]) return nullptr;
  for (const AircraftType& t : AIRCRAFT_TYPES)
    if (strcmp(t.icao, icao) == 0) return t.name;
  return nullptr;
}
