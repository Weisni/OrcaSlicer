"""Only admins and the configured identity may access selective inventory writes."""
def may_read(user,allowed):
    return bool(user and (getattr(user,'is_admin',False) or getattr(user,'id',None) in allowed))
def may_write(user,allowed,action):
    return bool(user and (getattr(user,'is_admin',False) or (action in ('native_apply','provider_apply','provider_delta','provider_job') and getattr(user,'id',None) in allowed)))
