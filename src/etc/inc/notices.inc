<?php
/*
 * notices.inc
 *
 * part of pfSense (https://www.pfsense.org)
 * Copyright (c) 2005 Colin Smith (ethethlay@gmail.com)
 * Copyright (c) 2005-2020 Rubicon Communications, LLC (Netgate)
 * All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

require_once("globals.inc");
require_once("functions.inc");
require_once("led.inc");

$notice_path = $g['tmp_path'] . '/notices';
$smtp_authentication_mechanisms = array(
	'PLAIN' => 'PLAIN',
	'LOGIN' => 'LOGIN');
/* Other SMTP Authentication Mechanisms that could be supported.
 * Note that MD5 is no longer considered secure.
 *	'GSSAPI' => 'GSSAPI ' . gettext("Generic Security Services Application Program Interface")
 *	'DIGEST-MD5' => 'DIGEST-MD5 ' . gettext("Digest access authentication")
 *	'MD5' => 'MD5'
 *	'CRAM-MD5' => 'CRAM-MD5'
*/

/****f* notices/file_notice
 * NAME
 *   file_notice
 * INPUTS
 *	 $id, $notice, $category, $url, $priority, $local_only
 * RESULT
 *   Files a notice and kicks off the various alerts, smtp, growl, system log, LED's, etc.
 *   If $local_only is true then the notice is not sent to external places (smtp, growl)
 ******/
function file_notice($id, $notice, $category = "General", $url = "", $priority = 1, $local_only = false) {
	/*
	 * $category - Category that this notice should be displayed under. This can be arbitrary,
	 * 	       but a page must be set to receive this messages for it to be displayed.
	 *
	 * $priority - A notice's priority. Higher numbers indicate greater severity.
	 *	       0 = informational, 1 = warning, 2 = error, etc. This may also be arbitrary,
	 */
	global $notice_path;
	if (!$queue = get_notices()) {
		$queue = array();
	}
	$queuekey = time();
	$toqueue = array(
				'id'		=> htmlentities($id),
				'notice'	=> htmlentities($notice),
				'url'		=> htmlentities($url),
				'category'	=> htmlentities($category),
				'priority'	=> htmlentities($priority),
			);
	while (isset($queue[$queuekey])) {
		$queuekey++;
	}
	$queue[$queuekey] = $toqueue;
	$queueout = fopen($notice_path, "w");
	if (!$queueout) {
		log_error(sprintf(gettext("Could not open %s for writing"), $notice_path));
		return;
	}
	fwrite($queueout, serialize($queue));
	fclose($queueout);
	log_error(sprintf(gettext("New alert found: %s"), $notice));
	/* soekris */
	if (file_exists("/dev/led/error")) {
		exec("/bin/echo 1 > /dev/led/error");
	}
	/* wrap & alix */
	led_normalize();
	led_morse(1, 'sos');
	if (!$local_only) {
		notify_all_remote($notice);
	}
	return $queuekey;
}

/****f* notices/get_notices
 * NAME
 *   get_notices
 * INPUTS
 *	 $category
 * RESULT
 *   Returns a specific notices text
 ******/
function get_notices($category = "all") {
	global $g;

	if (file_exists("{$g['tmp_path']}/notices")) {
		$queue = unserialize(file_get_contents("{$g['tmp_path']}/notices"));
		if (!$queue) {
			return false;
		}
		if ($category != 'all') {
			foreach ($queue as $time => $notice) {
				if (strtolower($notice['category']) == strtolower($category)) {
					$toreturn[$time] = $notice;
				}
			}
			return $toreturn;
		} else {
			return $queue;
		}
	} else {
		return false;
	}
}

/****f* notices/close_notice
 * NAME
 *   close_notice
 * INPUTS
 *	 $id
 * RESULT
 *   Removes a notice from the list
 ******/
function close_notice($id) {
	global $notice_path;
	require_once("util.inc");
	/* soekris */
	if (file_exists("/dev/led/error")) {
		exec("/bin/echo 0 > /dev/led/error");
	}
	/* wrap & alix */
	led_normalize();
	$ids = array();
	if (!$notices = get_notices()) {
		return;
	}
	if ($id == "all") {
		unlink_if_exists($notice_path);
		return;
	}
	foreach (array_keys($notices) as $time) {
		if ($id == $time) {
			unset($notices[$id]);
			break;
		}
	}
	foreach ($notices as $key => $notice) {
		$ids[$key] = $notice['id'];
	}
	foreach ($ids as $time => $tocheck) {
		if ($id == $tocheck) {
			unset($notices[$time]);
			break;
		}
	}
	if (count($notices) != 0) {
		$queueout = fopen($notice_path, "w");
		fwrite($queueout, serialize($notices));
		fclose($queueout);
	} else {
		unlink_if_exists($notice_path);
	}

	return;
}

/****f* notices/dump_xml_notices
 * NAME
 *   dump_xml_notices
 * INPUTS
 *	 NONE
 * RESULT
 *   Outputs notices in XML formatted text
 ******/
function dump_xml_notices() {
	if (file_exists("/cf/conf/use_xmlreader")) {
		require_once("xmlreader.inc");
	} else {
		require_once("xmlparse.inc");
	}
	global $notice_path, $listtags;
	$listtags[] = 'notice';
	if (!$notices = get_notices()) {
		return;
	}
	foreach ($notices as $time => $notice) {
		$notice['time'] = $time;
		$toput['notice'][] = $notice;
	}
	$xml = dump_xml_config($toput, 'notices');
	return $xml;
}

/****f* notices/are_notices_pending
 * NAME
 *   are_notices_pending
 * INPUTS
 *	 $category to check
 * RESULT
 *   returns true if notices are pending, false if they are not
 ******/
function are_notices_pending($category = "all") {
	global $notice_path;
	if (file_exists($notice_path)) {
		return true;
	}
	return false;
}

function notices_sendqueue() {
	global $g;
	$nothing_done_count = 0;
	$messagequeue = array();
	$growlerrorcount = 0;
	$growlskipped = 0;

	while(true) {
		$notifyqueue_lck = lock("notifyqueue", LOCK_EX);
		$nothing_done_count++;
		$smptcount = 0;
		$messages = array();
		if (file_exists("{$g['vardb_path']}/notifyqueue.messages")) {
			$messages = unserialize(file_get_contents("{$g['vardb_path']}/notifyqueue.messages"));
			$messagequeue = $messages;
			$messages['mails']['item'] = array(); // clear all items to be send
			file_put_contents("{$g['vardb_path']}/notifyqueue.messages", serialize($messages));
			unset($messages);
		}
		// clear lock before trying to send messages, so new one's can be added
		unlock($notifyqueue_lck);

		if (is_array($messagequeue['mails']['item'])) {
			$smtpmessage = "";
			$growlqueue = array();
			foreach($messagequeue['mails']['item'] as $mail) {
				switch ($mail['type']) {
					case 'mail':
						$smptcount++;
						$smtpmessage .= "\r\n" . date("G:i:s",$mail['time']) . " " . $mail['msg'];
						break;
					case 'growl':
						$message = date("G:i:s",$mail['time']) . " " . $mail['msg'];
						$nothing_done_count = 0;
						$growlqueue[] = $message;
						break;
					default:
						break;
				}
			}
			if (!empty($smtpmessage)) {
				$smtpmessageheader = sprintf(gettext("Notifications in this message: %s"), $smptcount);
				$smtpmessageheader .= "\n" . str_repeat('=', strlen($smtpmessageheader)) . "\n";
				$nothing_done_count = 0;
				notify_via_smtp($smtpmessageheader . $smtpmessage, true);
			}
			if (count($growlqueue) > 0) {
				$nothing_done_count = 0;
				foreach($growlqueue as $message) {
					if ($growlerrorcount < 3) {
						$ret = notify_via_growl($message, true);
						if (!empty($ret)) {
							$growlerrorcount++;
						}
					} else {
						$growlskipped++;
					}
				}
			}
		}
		if ($nothing_done_count > 6) {
			break;
		} else {
			sleep(10);
		}
	}
	if ($growlskipped > 0) {
		log_error("{$growlskipped} growl notifications were skipped due to to many errors: {$growlerrorcount}.");
	}
}

function notify_via_queue_add($message, $type='mail') {
	global $g;
	$mail = array();
	$mail['time'] = time();
	$mail['type'] = $type;
	$mail['msg'] = $message;
	$notifyqueue_lck = lock("notifyqueue", LOCK_EX);
	$messages = array();
	if (file_exists("{$g['vardb_path']}/notifyqueue.messages")) {
		$messages = unserialize(file_get_contents("{$g['vardb_path']}/notifyqueue.messages"));
	}
	if(is_array($messages)) {
		$messages['mails']['item'][] = $mail;
		file_put_contents("{$g['vardb_path']}/notifyqueue.messages", serialize($messages));
	}
	unset($messages);

	mwexec_bg('/usr/local/bin/notify_monitor.php');
	unlock($notifyqueue_lck);
}

/****f* notices/notify_via_smtp
 * NAME
 *   notify_via_smtp
 * INPUTS
 *	 notification string to send as an email
 * RESULT
 *   returns true if message was sent
 ******/
function notify_via_smtp($message, $force = false) {
	global $config, $g;
	if (platform_booting()) {
		return;
	}

	if (isset($config['notifications']['smtp']['disable']) && !$force) {
		return;
	}

	/* Do NOT send the same message twice, except if $force is true */
	if (!$force && file_exists("/var/db/notices_lastmsg.txt")) {
		$lastmsg = trim(file_get_contents("/var/db/notices_lastmsg.txt"));
		if ($lastmsg == $message) {
			return;
		}
	}

	/* Store last message sent to avoid spamming */
	@file_put_contents("/var/db/notices_lastmsg.txt", $message);
	if (!$force) {
		notify_via_queue_add($message, 'mail');
		$ret = true;
	} else {
		$ret = send_smtp_message($message, "{$config['system']['hostname']}.{$config['system']['domain']} - Notification", $force);
	}

	return $ret;
}

function send_smtp_message($message, $subject = "(no subject)", $force = false) {
	global $config, $g;
	require_once("Mail.php");

	if (isset($config['notifications']['smtp']['disable']) && !$force) {
		return;
	}

	if (!$config['notifications']['smtp']['ipaddress']) {
		return;
	}

	if (!$config['notifications']['smtp']['notifyemailaddress']) {
		return;
	}

	$to = $config['notifications']['smtp']['notifyemailaddress'];

	if (empty($config['notifications']['smtp']['username']) ||
	    empty($config['notifications']['smtp']['password'])) {
		$auth = false;
		$username = '';
		$password = '';
	} else {
		$auth = isset($config['notifications']['smtp']['authentication_mechanism'])
		    ? $config['notifications']['smtp']['authentication_mechanism']
		    : 'PLAIN';
		$username = $config['notifications']['smtp']['username'];
		$password = $config['notifications']['smtp']['password'];
	}

	$params = array(
		'host' => (isset($config['notifications']['smtp']['ssl'])
		    ? 'ssl://'
		    : '')
		    . $config['notifications']['smtp']['ipaddress'],
		'port' => empty($config['notifications']['smtp']['port'])
		    ? 25
		    : $config['notifications']['smtp']['port'],
		'auth' => $auth,
		'username' => $username,
		'password' => $password,
		'localhost' => $config['system']['hostname'] . "." .
		    $config['system']['domain'],
		'timeout' => !empty($config['notifications']['smtp']['timeout'])
		    ? $config['notifications']['smtp']['timeout']
		    : 20,
		'debug' => false,
		'persist' => false
	);

	if ($config['notifications']['smtp']['sslvalidate'] == "disabled") {
		$params['socket_options'] = array(
			'ssl' => array(
				'verify_peer_name' => false,
				'verify_peer' => false
		));
	}

	if ($config['notifications']['smtp']['fromaddress']) {
		$from = $config['notifications']['smtp']['fromaddress'];
	} else {
		$from = "pfsense@{$config['system']['hostname']}.{$config['system']['domain']}";
	}

	$headers = array(
		"From"    => $from,
		"To"      => $to,
		"Subject" => $subject,
		"Date"    => date("r")
	);

	$smtp =& Mail::factory('smtp', $params);
	$mail = $smtp->send($to, $headers, $message);

	if (PEAR::isError($mail)) {
		$err_msg = sprintf(gettext(
		    'Could not send the message to %1$s -- Error: %2$s'),
		    $to, $mail->getMessage());
		log_error($err_msg);
		return($err_msg);
	}

	log_error(sprintf(gettext("Message sent to %s OK"), $to));
	return;
}

/****f* notices/notify_via_growl
 * NAME
 *   notify_via_growl
 * INPUTS
 *	 notification string to send
 * RESULT
 *   returns true if message was sent
 ******/
function notify_via_growl($message, $force=false) {
	require_once("Net/Growl/Autoload.php");

	global $config, $g;

	if (isset($config['notifications']['growl']['disable']) && !$force) {
		return;
	}

	if (empty($config['notifications']['growl']['ipaddress'])) {
		return;
	}

	/* Do NOT send the same message twice */
	if (file_exists("/var/db/growlnotices_lastmsg.txt")) {
		$lastmsg = trim(file_get_contents("/var/db/growlnotices_lastmsg.txt"));
		if ($lastmsg == $message) {
			return;
		}
	}
	/* Store last message sent to avoid spamming */
	@file_put_contents("/var/db/growlnotices_lastmsg.txt", $message);

	if (!$force) {
		notify_via_queue_add($message, 'growl');
		return;
	}

	$ip = $config['notifications']['growl']['ipaddress'];

	if (!is_ipaddr($ip) && !(dns_check_record($ip, A) || dns_check_record($ip, AAAA))) {
		$err_msg = gettext(
		    "Growl IP Address is invalid. Check the setting in System Advanced Notifications.");
		log_error($err_msg);
		return($err_msg);
	}

	$hostname = $config['system']['hostname'] . "." . $config['system']['domain'];
	$password = $config['notifications']['growl']['password'];
	$name = $config['notifications']['growl']['name'];
	$notification = $config['notifications']['growl']['notification_name'];

	$options  = array(
		'host' => $ip,
		'protocol' => 'gntp',
		'timeout' => 20
	);

	try {
		$growl = Net_Growl::singleton($name, null, $password, $options);
		//$name = 'GROWL_NOTIFY_STATUS';
		$title = sprintf(gettext('%1$s (%2$s) - Notification'), $g['product_name'], $hostname);
		$growl->publish($name, $title, $message);
	} catch (Net_Growl_Exception $e) {
		$err_msg = sprintf(gettext(
		    'Could not send Growl notification to %1$s -- Error: %2$s'),
		    $ip, $e->getMessage());
		log_error($err_msg);
		return($err_msg);
	}

	return;
}

/****f* notices/register_via_growl
 * NAME
 *   register_via_growl
 * INPUTS
 *	 none
 * RESULT
 *   none
 ******/
function register_via_growl() {
	require_once("Net/Growl/Autoload.php");

	global $config;

	if (empty($config['notifications']['growl']['ipaddress'])) {
		return;
	}

	$ip = $config['notifications']['growl']['ipaddress'];

	if (!is_ipaddr($ip) && !(dns_check_record($ip, A) || dns_check_record($ip, AAAA))) {
		$err_msg = gettext(
		    "Growl IP Address is invalid. Check the setting in System Advanced Notifications.");
		log_error($err_msg);
		return($err_msg);
	}

	$name = $config['notifications']['growl']['name'];
	$password = $config['notifications']['growl']['password'];

	$app = new Net_Growl_Application(
		$name,
		null,
		$password
	);

	$options = array(
		'host' => $ip,
		'protocol' => 'gntp',
		'timeout' => 20
	);

	try {
		$growl = Net_Growl::singleton($app, null, null, $options);
		$growl->register();
	} catch (Net_Growl_Exception $e) {
		$err_msg = sprintf(gettext(
		    'Could not send register Growl on %1$s -- Error: %2$s'),
		    $ip, $e->getMessage());
		log_error($err_msg);
		return($err_msg);
	}

	return;
}

/* Notify via remote methods only - not via GUI. */
function notify_all_remote($msg) {
	notify_via_smtp($msg);
	notify_via_growl($msg);
}

?>