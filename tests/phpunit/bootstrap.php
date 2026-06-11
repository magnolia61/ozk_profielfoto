<?php
/**
 * Bootstrap voor standalone PHPUnit-tests van ozk_profielfoto.module.
 *
 * Stubt alle Drupal- en CiviCRM-afhankelijkheden zodat de module-functies
 * zonder draaiende Drupal/CiviCRM-stack getest kunnen worden.
 */

// ---------------------------------------------------------------------------
// Globals die door de stubs worden bijgehouden
// ---------------------------------------------------------------------------
$GLOBALS['_test_watchdog_log'] = [];
$GLOBALS['_test_api4_calls']   = [];
$GLOBALS['_test_api4_queue']   = [];   // stapel van return-waarden (FIFO)

// ---------------------------------------------------------------------------
// Drupal-constanten
// ---------------------------------------------------------------------------

define('WATCHDOG_EMERGENCY', 0);
define('WATCHDOG_ALERT',     1);
define('WATCHDOG_CRITICAL',  2);
define('WATCHDOG_ERROR',     3);
define('WATCHDOG_WARNING',   4);
define('WATCHDOG_NOTICE',    5);
define('WATCHDOG_INFO',      6);
define('WATCHDOG_DEBUG',     7);

// ---------------------------------------------------------------------------
// Drupal-stubs
// ---------------------------------------------------------------------------

function watchdog($type, $message, $vars = [], $severity = 6): void {
    $GLOBALS['_test_watchdog_log'][] = [
        'type'     => $type,
        'message'  => $message,
        'vars'     => $vars,
        'severity' => $severity,
    ];
}

function wachthond($channel, $level, $label, $data = NULL): void {}

function field_get_items($entity_type, $entity, $field_name): mixed {
    return FALSE;
}

function file_create_url(string $uri): string {
    return 'https://www.ozk_profielfoto.nl/' . ltrim($uri, '/');
}

// ---------------------------------------------------------------------------
// CiviCRM-stubs
// ---------------------------------------------------------------------------

function civicrm_initialize(): bool {
    return TRUE;
}

/**
 * Fake CiviCRM APIv4 Result: array-achtig én heeft first().
 * Kan als array geteld worden (count()), geïtereerd (foreach) en
 * numeriek benaderd worden ($result[0]).
 */
class FakeApiResult extends ArrayObject {
    public function first(): ?array {
        $arr = $this->getArrayCopy();
        return empty($arr) ? NULL : reset($arr);
    }
}

/**
 * Configurable civicrm_api4-stub.
 *
 * Gebruik $_test_api4_queue om return-waarden in volgorde te zetten:
 *   $GLOBALS['_test_api4_queue'] = [
 *       [['contact_id' => 42, 'contact_id.first_name' => 'Jan']],  // eerste call
 *       [],                                                          // tweede call
 *   ];
 * Als de queue leeg is, wordt [] teruggegeven.
 */
function civicrm_api4(string $entity, string $action, array $params = []): FakeApiResult {
    $GLOBALS['_test_api4_calls'][] = compact('entity', 'action', 'params');
    $rows = empty($GLOBALS['_test_api4_queue'])
        ? []
        : array_shift($GLOBALS['_test_api4_queue']);
    return new FakeApiResult((array) $rows);
}

// ---------------------------------------------------------------------------
// CiviCRM BAO-stub
// ---------------------------------------------------------------------------

class CRM_Core_BAO_UFMatch {
    public static function getContactId(int $uid): ?int {
        return NULL;
    }
}

// ---------------------------------------------------------------------------
// Base-extensie-stub
// ---------------------------------------------------------------------------

function base_find_allpart(int $contact_id): false {
    return FALSE;
}

// ---------------------------------------------------------------------------
// Module inladen (na alle stubs, zodat er geen fatale fouten zijn)
// ---------------------------------------------------------------------------

require_once __DIR__ . '/../../ozk_profielfoto.module';
