<?php declare(strict_types = 0);

namespace Modules\RaaMonitor\Includes;

use Zabbix\Widgets\CWidgetForm;

use Zabbix\Widgets\Fields\{
	CWidgetFieldIntegerBox,
	CWidgetFieldTextBox
};

class WidgetForm extends CWidgetForm {

	public function addFields(): self {
		return $this
			->addField(
				(new CWidgetFieldTextBox('server_host', _('RAA Server Host')))
					->setDefault('10.0.0.1')
					->setFlags(CWidgetFieldTextBox::FLAG_NOT_EMPTY)
			)
			->addField(
				(new CWidgetFieldIntegerBox('ws_port', _('WebSocket Port'), 1, 65535))
					->setDefault(4011)
			)
			->addField(
				(new CWidgetFieldIntegerBox('buffer_ms', _('Buffer (ms)'), 20, 2000))
					->setDefault(200)
			);
	}
}
