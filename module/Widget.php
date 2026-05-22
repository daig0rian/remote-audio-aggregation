<?php declare(strict_types = 0);

namespace Modules\RaaMonitor;

use Zabbix\Core\CWidget;

class Widget extends CWidget {

	public function getDefaultName(): string {
		return _('RAA Monitor');
	}
}
