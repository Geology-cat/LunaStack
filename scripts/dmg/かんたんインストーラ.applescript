-- LunaStack かんたんインストーラ
--
-- DMG の中の LunaStack.app を「アプリケーション」フォルダへコピーし、
-- インターネットから入手したときに付く「隔離」の印（com.apple.quarantine）を LunaStack だけから外して、
-- 最初の起動まで行う。macOS 全体の Gatekeeper の設定は変えない。
--
-- 画面の操作（on run）と実際の処理（installLunaStack）を分けてある。
-- 実際の処理は、試験のときにコピー先を差し替えて呼べる。

property appName : "LunaStack.app"
property bundleID : "com.lunastack.app"
property destinationFolder : "/Applications"

on run
	set sourceApp to (my folderOfThisScript()) & "/" & appName
	if not (my pathExists(sourceApp)) then
		display dialog "この DMG の中に " & appName & " が見つかりません。" & return & return & ¬
			"かんたんインストーラは、DMG を開いた中から実行してください。" buttons {"閉じる"} default button 1 with icon stop with title "LunaStack かんたんインストーラ"
		return
	end if

	set destinationApp to destinationFolder & "/" & appName
	set message to "LunaStack を「アプリケーション」フォルダにインストールします。" & return & return & ¬
		"1. アプリケーションフォルダへコピー" & return & ¬
		"2. LunaStack だけ、インターネットから入手したときの確認（Gatekeeper）を解除" & return & ¬
		"3. LunaStack を起動"
	if my pathExists(destinationApp) then
		set message to message & return & return & "すでにある LunaStack は、新しいものに置き換えます。"
	end if
	display dialog message buttons {"やめる", "インストール"} default button "インストール" cancel button "やめる" with icon note with title "LunaStack かんたんインストーラ"

	-- 起動中なら終わらせてから置き換える（使用中のアプリは正しく置き換えられない）。
	if my lunaStackIsRunning() then
		display dialog "LunaStack が起動しています。終了してからインストールします。" & return & "（処理中の作業があれば、先に保存してください）" buttons {"やめる", "終了して続ける"} default button "終了して続ける" cancel button "やめる" with icon caution with title "LunaStack かんたんインストーラ"
		tell application id bundleID to quit
		repeat 20 times
			if not (my lunaStackIsRunning()) then exit repeat
			delay 0.5
		end repeat
		if my lunaStackIsRunning() then
			display dialog "LunaStack を終了できませんでした。LunaStack を終了してから、もう一度実行してください。" buttons {"閉じる"} default button 1 with icon stop with title "LunaStack かんたんインストーラ"
			return
		end if
	end if

	try
		set installedApp to my installLunaStack(sourceApp, destinationFolder, false)
	on error errorMessage number errorNumber
		-- アプリケーションフォルダに書き込めない（管理者でない）ときは、管理者のパスワードで行う。
		try
			set installedApp to my installLunaStack(sourceApp, destinationFolder, true)
		on error errorMessage2 number errorNumber2
			if errorNumber2 is -128 then return -- パスワードの入力をやめた
			display dialog "インストールできませんでした。" & return & return & errorMessage2 buttons {"閉じる"} default button 1 with icon stop with title "LunaStack かんたんインストーラ"
			return
		end try
	end try

	do shell script "open " & quoted form of installedApp
	display dialog "インストールが終わりました。LunaStack を起動しました。" & return & return & ¬
		"次からは「アプリケーション」フォルダ（または Launchpad）から起動できます。" & return & ¬
		"使い方は、DMG の中の「LunaStack 使い方ガイド.pdf」をご覧ください。" buttons {"OK"} default button 1 with icon note with title "LunaStack かんたんインストーラ" giving up after 30
end run

-- 実際の処理。sourceApp を destination フォルダへコピーし、隔離の印を外す。コピーしたアプリのパスを返す。
-- withAdmin が true なら管理者の権限で行う（パスワードを求められる）。
on installLunaStack(sourceApp, destination, withAdmin)
	set destinationApp to destination & "/" & appName
	-- 置き換えるのは、コピー先の LunaStack.app そのものだけ（パスは固定の名前で組み立てる）。
	set commands to "set -e; " & ¬
		"if [ -e " & quoted form of destinationApp & " ]; then rm -rf " & quoted form of destinationApp & "; fi; " & ¬
		"/usr/bin/ditto " & quoted form of sourceApp & " " & quoted form of destinationApp & "; " & ¬
		"/usr/bin/xattr -dr com.apple.quarantine " & quoted form of destinationApp & " 2>/dev/null || true"
	if withAdmin then
		do shell script commands with administrator privileges
	else
		do shell script commands
	end if
	return destinationApp
end installLunaStack

-- まだ一度も起動したことのない Mac では application id の問い合わせが失敗しうるので、そのときは「起動していない」。
on lunaStackIsRunning()
	try
		return application id bundleID is running
	on error
		return false
	end try
end lunaStackIsRunning

on folderOfThisScript()
	set scriptPath to POSIX path of (path to me)
	return do shell script "/usr/bin/dirname " & quoted form of scriptPath
end folderOfThisScript

on pathExists(p)
	try
		do shell script "/bin/test -e " & quoted form of p
		return true
	on error
		return false
	end try
end pathExists
