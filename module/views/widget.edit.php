<?php declare(strict_types = 0);

/**
 * RAA Monitor widget edit form view.
 *
 * @var CView  $this
 * @var array  $data
 */

$form = new CWidgetFormView($data);

$form
	->addField(
		new CWidgetFieldTextBoxView($data['fields']['server_host'])
	)
	->addField(
		new CWidgetFieldIntegerBoxView($data['fields']['ws_port'])
	)
	->addField(
		new CWidgetFieldIntegerBoxView($data['fields']['buffer_ms'])
	)
	->show();
